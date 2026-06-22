# Design Doc: Bridge登録MQのkmod常駐キューによる代替

> Status: Proposed
> Scope: kmod (minor-update-label) + agnocastlib + bridge_manager
> Related: [docs/message_queue.md#how-message-queue-is-used-in-agnocast-bridge](docs/message_queue.md)

## 1. 背景と目的 (Background & Objectives)

Agnocast Bridge は、Publisher/Subscriber/Service の生成タイミングで「Bridge を張るべきトピック/サービス」を Bridge Manager に登録している。現在この登録経路には POSIX Message Queue (`/agnocast_bridge_manager@-1[_d<ROS_DOMAIN_ID>]`) が使われており、エンティティ側が `mq_open(O_CREAT | O_WRONLY | O_NONBLOCK)` で MQ を作成して `MqMsgPerformanceBridge`（約 1.1 KB / トピック名・メッセージ型名・target_id・direction・is_service・サービス用 union を保持）を 1 回だけ送信する片方向通信である。

この構成には次の構造的な問題がある。

1. **MQ ファイルがクリーンアップされない**: 生成側 (Pub/Sub) は MQ を所有しないので unlink できない。所有者である Bridge Manager は自身の `IpcEventLoopBase::cleanup_resources()` で `mq_unlink` するが、SIGKILL や OOM など正常終了経路を通らない場合に `/dev/mqueue` 上にファイルが残り、再起動時の `fs.mqueue.queues_max` 上限を圧迫する事故が継続的に報告されている。
2. **`fs.mqueue.*` リソースに対する依存**: ホスト側 sysctl の値に挙動が左右され、CI / 実車間で再現性のないバグが生じる。
3. **Bridge 起動レースの存在**: Bridge デーモンは初回プロセス側で `fork()` 起動されるため、最初のエンティティが MQ を `O_CREAT` し終えた直後 (10 ms 〜 20 ms オーダー) に Bridge が `mq_open` で受信側を開ける。MQ がカーネル内バッファとして機能しているため致命的な取りこぼしは起きていないが、リソース管理を MQ に依存している以上、このレースの上に常に立っている。

本設計の目的は、`MqMsgPerformanceBridge` を運ぶ POSIX MQ を **kmod 常駐の per-IPC-namespace キュー** に置き換え、`mq_*` への依存を Bridge 登録経路から完全に切ることである。これにより、リソースは OS（モジュールアンロード）が必ず回収する形となり、`fs.mqueue.*` 由来のバグクラスが排除される。

実装方針として、既存の `MqMsgPerformanceBridge` 構造体はそのまま維持し、MQ の代わりに kmod 経由でやり取りする。kmod はメッセージの中身を解釈せず、opaque な固定サイズバッファとして保持・転送する。

> Out of scope: Subscriber `publish notification` 用の MQ (`/agnocast@<topic>@<id>`) はホットパスであり、本設計の対象外。`/agnocast_daemon_bridge_perf` (cross-NS daemon → bridge_manager) も同じ問題を抱えるが、後述 §8 で同じパターンの拡張先として言及する。

## 2. 要求仕様 (Requirements)

### R1. リソースの自動回収

SIGKILL を含む異常終了でも、Bridge 通知に使うリソースが OS に回収されること。具体的には `/dev/mqueue/` 配下にユーザー空間が作成するファイルが存在しない構成にする。kmod が確保する内部メモリは、最終的にモジュールアンロード時に必ず解放される。

### R2. 起動時間を悪化させないこと

アプリケーション起動の最初の 10 ms 〜 20 ms に大量のエンティティ生成が走るため、Bridge 通知の送信は **Bridge が立ち上がっているかに関わらずノンブロッキングで完了** すること。Bridge 起動完了を待つような同期は許容しない。

### R3. データペイロードを運べること

通知は単なるイベントではなく、トピック名 / メッセージ型名 / target_id / direction / サービス用補助情報を含む構造体 (`MqMsgPerformanceBridge`, ~1.1 KB) をそのまま運べること。

### R4. AGNOCAST_BRIDGE_MODE による制御の維持

`AGNOCAST_BRIDGE_MODE=off` 時は通知経路自体を呼ばない、という現行のユーザー空間ゲートをそのまま残せること。

### R5. Bridge 起動以前のエンティティ通知をバッファリングできること

最初の Agnocast プロセス内で、`fork()` 完了前にエンティティが生成された場合でも通知が失われないこと。Bridge は起動直後にバッファ済みの通知を一括取得できる。

### R6. Bridge 終了後の挙動

現状の運用では同一 IPC-namespace 内で Bridge は再生成されないが、kmod 側にはその制約を焼き込まない。**Bridge 終了後に送信されたメッセージはキューに保持され、次に Bridge が立ち上がったときに受信される**こと。これは MQ ベースの現行実装が `/dev/mqueue/` 上のファイルを介して結果的に提供している性質 (Bridge 不在期間中の `mq_send` がカーネル内バッファに蓄積され、次の `mq_open(O_RDONLY)` で読み出せる) を新設計でも維持する、という意味である。

### R7. kmod ABI 後方互換

既存 ioctl はすべて据え置く。新規 ioctl の追加のみで成立させ、minor バージョンアップに収める。

### R8. ロック衝突の最小化

通知は 1 process : 1 IPC-namespace 内で N 個の publisher/subscriber が同時に送る可能性がある (N producers / 1 consumer)。この経路がデータプレーンのロック (per-topic `topic_rwsem`, `global_htables_rwsem` の write 取得など) と干渉しないこと。

## 3. 既存資産の確認 (Existing Building Blocks)

新規実装で参照すべき先行パターンが kmod 内にすでに存在する。

| パターン | 既存実装 | 新設計での再利用 |
| --- | --- | --- |
| Per-IPC-namespace スコープの bridge トラッキング | `bridge_htable` / `agnocast_ioctl_add_bridge` / `is_performance_bridge_manager` flag in `process_info` | 「現在 alive な bridge_manager が誰か」を判定する既存ソース |
| Per-IPC-namespace データを「最後の参照者が exit したとき」に回収 | `agnocast_process_exit_cleanup` 内で `topic_wrapper` / `bridge_info` を、参照を持つ pub/sub がそれぞれ 0 になったところで kfree | `bridge_msg_queue` も同じパスで `get_process_num(ipc_ns) == 0` のときに kfree (§5.6.6) |
| 2-phase get/commit ioctl で kmod のキューを drain する | `AGNOCAST_GET_EXIT_PROCESS_CMD` / `agnocast_commit_exit_process` (ロックを保持したまま `copy_to_user` しない) | receiver fd の `.read` で同型の 2-phase を採用 (§5.7) |
| 2 層ロック階層 | `global_htables_rwsem` (top-level) + `topic_wrapper->topic_rwsem` (per-instance) | `bridge_msg_queue` は per-instance rwsem を持たず、`global_htables_rwsem` のみで保護 (ホットパスではないため) |

## 4. アプローチ比較 (Evaluated Approaches)

### A. 現状維持 (POSIX MQ)

- Pros: 実装済み・追加コストゼロ。
- Cons: R1 を満たさない。`fs.mqueue.queues_max` 由来の障害が再発し続ける。**却下**

### B. Abstract Namespace UDS

`SOCK_STREAM` または `SOCK_DGRAM` を `\0agnocast_bridge/<ipc_ns>` で listen し、エンティティが connect → send。プロセス終了で OS が自動回収する。

- Pros: 既に bridge daemon の死活監視 (`§4.1` of `design-doc.context.md`) で同じパターンを採用済み。OS による自動回収。実装は user-space で完結。
- Cons:
  - **R2 違反のリスク**: connect-then-send は Bridge が listen を始めるまで「`ECONNREFUSED` でリトライする」スピンが必要になる。`mq_send` のように「先に作ってバッファに置く」セマンティクスが得られない。SOCK_DGRAM unconnected であっても受信側が bind するまでは送れない (sendto が `ECONNREFUSED` を返す)。
  - **Network namespace 依存**: abstract socket 名は net-ns スコープであり、Agnocast の他の経路 (IPC-ns スコープ) と粒度が一致しない。コンテナ運用で混乱を招く。
  - N producer → 1 consumer の Bridge 側受信バッファ競合。
- 結論: **却下**。R2 を満たすための「先行送信」の挙動を再現するには結局 user-space 側に再試行ループを入れる必要があり、現状の `mq_send` リトライと変わらない。

### C. 共有メモリ + eventfd

agnocast の mempool とは別に、IPC-ns 単位で `tmpfs` 上の共有 ring buffer + eventfd を用意する。

- Pros: kmod を変更しない。
- Cons: ring buffer 管理 (head/tail のレースフリーな更新、フラッシュ済み記録) を完全に user-space で書く必要があり、bug の入り口が増える。リソース回収 (tmpfs ファイルの unlink) の問題は MQ と同じパターンで再発する。**却下**

### D. kmod 常駐キュー + `.poll` 通知（**選定**）

新規 ioctl を 2 本 (`REGISTER` / `CREATE_RECEIVER`) kmod に追加し、`MqMsgPerformanceBridge` のペイロードを per-IPC-namespace の FIFO キューに格納する。送信側は ioctl 1 回、受信側は `anon_inode_getfd()` で作成した receiver fd への `read()` で行う。

- Pros:
  - R1 を完全に満たす（カーネル内メモリのみ。モジュールアンロードで全解放される）。
  - R2 を満たす（登録は `global_htables_rwsem.write` + list_add_tail で O(1)、Bridge 不在でもキューに溜まる）。
  - R5 を満たす（Bridge 起動時に receiver fd を作成し、バッファ済みエントリを一括 read）。
  - キューの生存期間管理が既存 `topic_wrapper` / `bridge_info` と同じモデルに揃う (§5.6)。「誰がキューを抱えるか」という独自 ownership を導入せず、IPC-ns 内の Agnocast プロセスがすべて exit したときに `agnocast_process_exit_cleanup` が自動に kfree する。`free_ipc_ns` への依存も receiver fd の refcount も不要。
  - `MqMsgPerformanceBridge` を変更せずそのまま使える。kmod は opaque バイト列として扱うだけで、C++ 固有の型 (enum class 等) を kmod ヘッダに持ち込まない。
  - receiver fd は SIGKILL 時もプロセス終了でカーネルが自動 close するため、Bridge 異常終了時のリソースリークがない。
- Cons:
  - kmod ABI が増える（minor bump）。設計ミスを後から直しにくい。
  - kmod 側に「Bridge 用の薄いメッセージブローカー」が常駐することになる。
- 結論: **採用**。kmod は既に bridge_htable と `is_performance_bridge_manager` で「Bridge とは何者か」を知っており、本キューはその責務の自然な拡張である。共有メモリ＋通知やソケットでは取れない「OS による絶対的なリソース回収」が要件 R1 から不可避である。

### Bridge 期間モデルの選定

ドラフト中で 3 つの選択肢が議論されていた。

| モデル | 内容 | 採用可否 |
| --- | --- | --- |
| (a) Bridge 存在中に作成された entity のみ | Bridge 起動を待ってから送信する | R2 違反 |
| (b) Bridge 起動以前の entity も含む | Bridge 不在でも送信、Bridge は起動時にバッファを受信 | **採用** |
| (c) Bridge 起動時に kmod から現存 entity の snapshot を取る | メッセージ送信を廃止し、`AGNOCAST_GET_TOPIC_*_INFO` 系を拡張 | 却下: スナップショット取得は Bridge 起動時の一度限りであり、Bridge が稼働している間に新たに生成されたエンティティへの対応が別途必要になる。結局 (b) と同等のプッシュ機構を残したうえでスナップショット API も追加することになり、複雑さが (b) 単独より増す。またメッセージ型名・shadow node 情報は kmod が IPC を行う上では不要なデータであり、Bridge が alive でない状態でもすべての pub/sub エントリに保持し続けるメモリコストが生じる。 |

(b) を採用することで、ペイロードと所有者がそれぞれ「user-space で完結する Bridge 固有メタ」「kmod が責務を持つ FIFO の物理的存在」に綺麗に分離できる。

## 5. 詳細設計 (Proposed Architecture)

### 5.1. 新規 ioctl

| 番号 | 名前 | 方向 | 用途 |
| --- | --- | --- | --- |
| `_IOW(0xA6, 28, struct ioctl_send_msg_to_bridge_args)` | `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` | エンティティ → kmod | 現行の `send_mq_message` の代替。任意の Bridge 向けメッセージ。1 件を opaque な可変長バイト列 (size と payload のペア) としてキュー末尾へ追加する。kmod は中身を一切解釈しない。 |
| `_IO(0xA6, 29)` | `AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` | kmod → bridge_manager | Bridge Manager 起動時に 1 回呼び出し、Bridge 向けメッセージ受信専用の anonymous fd を生成して返す (戻り値が新 fd)。この fd を `epoll` に登録し、EPOLLIN 発火後に `read()` で datagram を 1 件ずつ取り出す。 |

`AGNOCAST_NOTIFY_BRIDGE_SHUTDOWN_CMD` (既存) と process exit fastpath (既存 `agnocast_process_exit_cleanup`) の 2 つは bridge_manager 終了の検知点として再利用する。新規の終了用 ioctl は追加しない。

### 5.2. ペイロード型

kmod は Bridge 向けメッセージの中身を一切解釈しない。ペイロードは **opaque な可変長バイト列**として扱い、kmod 側には `MqMsgPerformanceBridge` の構造体定義もサイズ定数も持ち込まない。これにより、将来 Bridge メッセージのレイアウトを拡張・変更しても kmod を minor bump しないで済む。

```c
/* agnocast.h */

/* Bridge 向け 1 メッセージの上限。現行の MqMsgPerformanceBridge (~1.1 KB) より十分大きく
 * とり、将来のメタデータ追加にリザーブを持たせる。kmod は上限としてしか見ない。 */
#define MAX_BRIDGE_MSG_SIZE 2048

struct ioctl_send_msg_to_bridge_args
{
  uint32_t size;                       /* 実際に有効なバイト数 (<= MAX_BRIDGE_MSG_SIZE) */
  uint8_t  payload[MAX_BRIDGE_MSG_SIZE];
};
```

user-space 側は既存の `MqMsgPerformanceBridge` をそのまま維持し、ioctl 呼び出し時に `size = sizeof(MqMsgPerformanceBridge)` として `memcpy` でペイロードフィールドへ詰める。

```cpp
// agnocastlib 側 (C++)
static_assert(
  sizeof(MqMsgPerformanceBridge) <= MAX_BRIDGE_MSG_SIZE,
  "MqMsgPerformanceBridge exceeds kmod MAX_BRIDGE_MSG_SIZE");

struct ioctl_send_msg_to_bridge_args args = {};
args.size = sizeof(MqMsgPerformanceBridge);
std::memcpy(args.payload, &msg, args.size);
ioctl(agnocast_fd, AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, &args);
```

receiver fd を通じて受け取った側は `read(2)` の戻り値 (メッセージサイズ) を見て `MqMsgPerformanceBridge` にキャストする。

> kmod をロードした状態でメッセージレイアウトを変えることは許容される (`MAX_BRIDGE_MSG_SIZE` を越えない限り)。`MAX_BRIDGE_MSG_SIZE` を越える拡張が必要になったときのみ kmod の minor bump が起きる。

### 5.3. kmod 内部データ構造

```c
struct bridge_msg_entry
{
  uint32_t size;                   /* ペイロードの有効バイト数 */
  struct list_head node;
  uint8_t payload[];               /* flexible array; 長さは size バイト */
};

struct bridge_msg_queue
{
  const struct ipc_namespace * ipc_ns;
  struct list_head entries;        /* FIFO, send: tail / read: head */
  uint32_t entry_count;
  wait_queue_head_t wait;          /* receiver fd の .poll が poll_wait する */
  struct hlist_node hnode;
};

DECLARE_HASHTABLE(bridge_msg_queue_htable, 6);  /* IPC-ns 数は通常 1 〜 数個 */
```

- 1 IPC-namespace につき 1 個。最初の `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` または `AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` が来た時点で lazy に作成。
- 既存 `bridge_htable` とは別の構造（`bridge_htable` は per-topic、こちらは per-IPC-ns）。
- エントリは `kmalloc(sizeof(struct bridge_msg_entry) + size, GFP_KERNEL)` で実サイズ分だけ確保し、`MAX_BRIDGE_MSG_SIZE` 全部を使うわけではない。
- 主要フィールドは `topic_wrapper` / `bridge_info` と同じく **`global_htables_rwsem` の下で保護される** (§5.7)。`entries` と `entry_count` も同一ロックに乗るため spinlock 不要。
- `wait` のみ kernel built-in の wait queue lock を使う (`wake_up_interruptible`)。

#### receiver fd

`AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` は `anon_inode_getfd()` を用いて anonymous fd を作成する。`file->private_data` にキューへのポインタを持ち、以下の file operations を持つ。

```c
static const struct file_operations bridge_msg_receiver_fops = {
  .owner   = THIS_MODULE,
  .read    = bridge_msg_receiver_read,    /* キューから 1 メッセージ取り出す (datagram) */
  .poll    = bridge_msg_receiver_poll,    /* epoll 用: キュー非空で POLLIN */
  .release = bridge_msg_receiver_release,
};
```

- **`.read`**: **datagram セマンティクス**。1 回の `read` でキュー先頭の 1 メッセージを取り出し、そのメッセージの `size` バイトをユーザーバッファへ `copy_to_user`。ユーザーバッファがメッセージサイズより小さいと `-EMSGSIZE` を返しエントリはキューに残す (UDP ソケットと同じ振る舞い)。`down_read(&global_htables_rwsem)` でキューポインタの生存を保証し、エントリをローカルに取り出して `up_read` してから `copy_to_user`。O_NONBLOCK 時はキューが空なら `EAGAIN` を返す。
- **`.poll`**: `down_read(&global_htables_rwsem)` で queue ポインタを取り、`poll_wait(file, &q->wait, wait)` の後、`list_empty(&q->entries) ? 0 : (POLLIN | POLLRDNORM)` を返す。
- **`.release`**: キュー自体は破棄しない（次の Bridge Manager が別の receiver fd を作成して受信を継続できるようにする）。

### 5.4. キュー上限と溢れ時の動作

```c
#define MAX_BRIDGE_MSG_QUEUE_LEN 1024  /* per IPC-namespace */
```

- 1 エントリの実サイズは `size` だけのため、`MqMsgPerformanceBridge` (~1.1 KB) リクエストのみ送られるケースで上限到達時 ~1.1 MB / IPC-ns。他のメッセージ種別が併用されるとさらに小さくなる。
- 上限に達した状態で送信が来た場合は **エンキューせず `-ENOSPC` を返す**。`dev_warn_ratelimited` で警告。
- これは現行の `mq_send` が EAGAIN を返した場合の user-space 側挙動 (100 回 × 100 ms リトライ後にエラーログ) と等価か、それより安全なフォールバック挙動 (リトライしないので送信のレイテンシが青天井にならない)。

`MAX_BRIDGE_MSG_QUEUE_LEN` は現行の `PERFORMANCE_BRIDGE_MQ_MAX_MESSAGES = 256` を 4 倍程度にした安全側の値。Autoware 規模 (~数百ノード) で、Bridge 起動前バッファとして十分なヘッドルームを確保する。

### 5.5. 通知機構: receiver fd

`AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` が返す anonymous fd を Bridge Manager が `epoll` に登録する。`agnocast` キャラクタデバイス本体の `file_operations` には変更を加えない。

#### 登録側 (エンティティプロセス)

`AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` の ioctl ハンドラは、エントリをキュー末尾へ追加した後に `wake_up_interruptible(&q->wait)` を呼び、Bridge Manager の `epoll_wait` を起こす。

#### 送信側 (エンティティプロセス)

`AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` の ioctl ハンドラは、エントリをキュー末尾へ追加した後に `wake_up_interruptible(&q->wait)` を呼び、Bridge Manager の `epoll_wait` を起こす。

#### 受信側 (Bridge Manager)

```cpp
// Bridge Manager 起動シーケンス
const int receiver_fd =
  ioctl(agnocast_fd_, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
if (receiver_fd < 0) { /* fatal */ }
event_loop_.register_bridge_msg_receiver(receiver_fd,
  [this]() { this->on_bridge_msgs(); });
```

`IpcEventLoopBase` は `receiver_fd` を `epoll(EPOLLIN | EPOLLET)` で監視する。EPOLLIN 発火後、`on_bridge_msgs()` は `read(receiver_fd, buf, sizeof(buf))` を EAGAIN が返るまでループし、受け取った 1 メッセージをサイズに基づいて適切な型として処理する。

```cpp
void on_bridge_msgs()
{
  alignas(MqMsgPerformanceBridge) std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> buf;
  for (;;) {
    ssize_t n = read(receiver_fd_, buf.data(), buf.size());
    if (n <= 0) break;  // EAGAIN または EOF
    if (n == sizeof(MqMsgPerformanceBridge)) {
      MqMsgPerformanceBridge msg;
      std::memcpy(&msg, buf.data(), n);
      process_bridge_registration(msg);  // 既存の on_mq_request 相当
    } else {
      RCLCPP_WARN(logger_, "unknown bridge message size: %zd", n);
    }
  }
}
```

この設計により `IpcEventLoopBase` の Primary MQ (`create_mq_name_for_bridge`) を**廃止**でき、`mq_open` / `mq_close` / `mq_unlink` を含む MQ 初期化パスがすべて削除される。epoll ループ本体の構造変更は `register_aux_mq` → `register_bridge_msg_receiver` の差し替えのみ。

### 5.6. ライフサイクル

**方針**: キューの生存期間は既存の `topic_wrapper` / `bridge_info` と同一のモデルに揃える。すなわち、IPC-ns 内に Agnocast プロセスが 1 つでも生存する限りキューも生存し、全プロセスが exit したタイミングで自動回収される。これにより「誰がキューを抱えるか」というownership議論を避け、`free_ipc_ns` や fd refcount に依存せずに済む。

#### 5.6.1. キュー生成

最初の `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` または `AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` を受けた時点で、対応する IPC-ns の `bridge_msg_queue` を `global_htables_rwsem.write` の下で kmalloc し、`bridge_msg_queue_htable` に登録する。

#### 5.6.2. Bridge Manager 起動シーケンス

1. `acquire_agnocast_resources_for_bridge()` 内で `AGNOCAST_ADD_PROCESS_CMD(is_performance_bridge_manager=true)` を発行 (既存)。
2. `AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` を呼び出して receiver fd を取得し、`IpcEventLoopBase` の epoll に登録。
3. `spin_once` の最初の呼び出しでバッファ済みメッセージを受信し、現行 `on_mq_request` と同等のロジックでブリッジを生成。

R5 の「Bridge 起動以前の entity も含む」要件はこれで満たされる (メッセージはすでに kmod 内のキューに溜まっているため、起動直後の read で取り出せる)。

#### 5.6.3. Bridge Manager 正常終了

`AGNOCAST_NOTIFY_BRIDGE_SHUTDOWN_CMD` は **キューをクリアしない**。`process_info->is_performance_bridge_manager` を `false` に倒すだけ (既存挙動)。Bridge Manager プロセスが保持していた receiver fd は `close(2)` またはプロセス終了で OS が自動回収する。`bridge_msg_queue` 本体は保持され、以降に送信されたメッセージとあわせて、次に同 IPC-ns で `AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` を発行した Bridge が受信することになる。

> 現状の運用では同 IPC-ns に Bridge は再生成されない (R6) が、kmod ABI として「Bridge は再起動可能」「Bridge 不在期間中のメッセージも次の Bridge で受け取れる」を保証しておくことで、将来の運用変更（例: Bridge crash 時の自動再 fork）を妨げない。

#### 5.6.4. Bridge Manager 異常終了

`agnocast_process_exit_cleanup` 経由の既存パスで `process_info->is_performance_bridge_manager == true` だった場合も同様に **キューをクリアしない**。receiver fd は SIGKILL 時もプロセス終了でカーネルが自動 close する (anonymous fd はプロセスの fd table に属するため)。

> Bridge が死亡した瞬間、キューに残っていたメッセージは「死亡した Bridge が読みきれなかった分」である。新しい Bridge から見れば「Bridge 不在期間中のメッセージ」と区別する意味はないため、まとめて次の Bridge へ渡すのが自然。Bridge 側にはすでに `check_and_remove_request_cache` / `remove_invalid_requests` による dead-target 検知 (`get_subscriber_qos`/`get_publisher_qos` ioctl をプローブにして死んだ entity を弾く) が実装済みなので、古いエントリが新 Bridge を誤動作させる懸念はない。

#### 5.6.5. キュー上限と Bridge 不在期間中の保護

Bridge 終了後にキューがクリアされないため、「Bridge が二度と起動しない」運用 (現状の Autoware 動作) では、送信が来るたびにキューが伸び続け、最終的に §5.4 の `MAX_BRIDGE_MSG_QUEUE_LEN = 1024` で頭打ちになる。上限到達後の送信は `-ENOSPC` で静かに落とされる (運用上の検知性は `dev_warn_ratelimited`)。これは現状の MQ 実装が `MAX_MESSAGES = 256` で先に頭打ちになる挙動と等価で、本設計が悪化させるシナリオではない。

#### 5.6.6. キュー破棄 (主経路)

`agnocast_process_exit_cleanup` に 1 ブロック追加し、`global_htables_rwsem.write` を保持したまま以下を行う。

```c
// agnocast_process_exit_cleanup() の末尾に追加
if (get_process_num(proc_info->ipc_ns) == 0) {
  struct bridge_msg_queue * q;
  struct hlist_node * tmp;
  int bkt;
  hash_for_each_safe(bridge_msg_queue_htable, bkt, tmp, q, hnode) {
    if (!ipc_eq(q->ipc_ns, proc_info->ipc_ns)) continue;
    hash_del(&q->hnode);
    /* entries はすでに receiver fd がいないため send はもう来ない。
     * 未読エントリをすべて解放。 */
    struct bridge_msg_entry * e, * ne;
    list_for_each_entry_safe(e, ne, &q->entries, node) {
      list_del(&e->node);
      kfree(e);
    }
    kfree(q);
  }
}
```

これは既存 `topic_wrapper` が `agnocast_get_size_pub_info_htable() == 0 && agnocast_get_size_sub_info_htable() == 0` で kfree されるパターンと同型。「最後の負け」を IPC-ns スコープで判定してリソースを回収する。receiver fd は Bridge プロセス exit 時に先行して close されているため、このタイミングで alive な fd は存在しない (receiver fd を抱えたプロセス = Bridge も Agnocast プロセス = get_process_num にカウントされる)。

#### 5.6.7. キュー破棄 (safety net)

モジュールアンロード時にさらに `agnocast_exit_free_data` で `bridge_msg_queue_htable` をループ削除し、残っているキューとエントリを kfree する (万一 §5.6.6 を通らずに生き残っていた場合に備えた保険)。R1 を満たす最終リソース回収点。

### 5.7. ロック設計

既存 kmod の 2 層ロック階層 (`global_htables_rwsem` → `topic_wrapper->topic_rwsem`) に乗る。`bridge_msg_queue` は `topic_rwsem` 相当の per-instance rwsem を持たず、`global_htables_rwsem` だけで保護する。これで十分な理由は、ホットパスではないため read-side との同期ずらしコストを許容できるから。

| 操作 | 取るロック |
| --- | --- |
| `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` (send) | `down_write(&global_htables_rwsem)` — キュー初回生成も同じロック下で行う |
| receiver fd `.read` | `down_read(&global_htables_rwsem)` で queue ポインタの存在を保証し、1 エントリをローカルに取り出し → `up_read` → ロック外で `copy_to_user`。 |
| receiver fd `.poll` | `rcu_read_lock` または `down_read(&global_htables_rwsem)` → `list_empty()` チェック |
| `AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` | `down_write(&global_htables_rwsem)` |
| `agnocast_process_exit_cleanup` でのキュー kfree (§5.6.6) | 既存の `down_write(&global_htables_rwsem)` に相乗り、追加ロック不要 |

**send path を write ロックにしてよい理由**: 現行 MQ の push も `mq_send` 内部で同等のシリアライゼーションを起こしており、ホットパスではない。`AGNOCAST_ADD_SUBSCRIBER_CMD` / `AGNOCAST_ADD_PUBLISHER_CMD` も同じ write を取っており、entity 生成パスのロック粒度と一致している。データプレーン (`publish` / `receive`) は一切巻き込まれないため R8 は満たされる。

**read path を read ロック + lock-free リスト移動にしない理由**: receiver fd は 1 プロセス (Bridge Manager) しか使わないため read 同士の競合はない。`down_read` で queue ポインタの dangling を防ぐだけで足りる。

**`copy_to_user` をロック外で行う**: read パスでは `down_read` 中に list からエントリをローカルに取り出し、`up_read` 後に `copy_to_user`。`agnocast_ioctl_get_exit_process` の 2-phase 設計と同型。

### 5.8. user-space 側の差分

#### 5.8.1. `agnocast_publisher.cpp` / `agnocast_subscription.cpp` / 各種 service 生成パス

`send_mq_message(...)` 経由の `mq_open(O_CREAT|O_WRONLY|O_NONBLOCK)` → `mq_send` → `mq_close` のシーケンスを `ioctl(agnocast_fd, AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, &args)` の 1 回呼び出しに置き換える。

```cpp
// agnocast_bridge_node.hpp (after)
inline void send_performance_pubsub_bridge_registration_by_type_name(
  const std::string & topic_name, topic_local_id_t id, const std::string & message_type_name,
  BridgeDirection direction)
{
  auto [msg, reason] = BridgeRegistrationMsgBuilder()
                         .set_direction(direction)
                         .set_is_service(false)
                         .set_message_type(message_type_name.c_str())
                         .set_topic_name(topic_name.c_str())
                         .set_pubsub_target_id(id)
                         .build_performance_message();
  if (!reason.empty()) { /* 既存と同じエラー処理 */ }

  static_assert(
    sizeof(msg) <= MAX_BRIDGE_MSG_SIZE, "MqMsgPerformanceBridge exceeds kmod MAX_BRIDGE_MSG_SIZE");
  struct ioctl_send_msg_to_bridge_args args = {};
  args.size = sizeof(msg);
  std::memcpy(args.payload, &msg, args.size);

  if (ioctl(agnocast_fd, AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, &args) < 0) {
    if (errno == ENOSPC) {
      /* キューが満杯。現行 mq_send EAGAIN 後の挙動と同じく warn して諦める */
    } else {
      /* それ以外は致命的 */
    }
  }
}
```

`send_mq_message`, `BRIDGE_MQ_SEND_MAX_RETRIES`, `usleep` リトライループは削除する。

#### 5.8.2. `IpcEventLoopBase`

- Primary MQ (`create_mq_name_for_bridge`) の `setup_mq` / `mq_close` / `mq_unlink` を削除。
- Bridge Manager 起動時に `ioctl(agnocast_fd_, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD)` を呼び出して receiver fd を取得するセットアップを追加。
- `set_mq_handler(EventCallback)` を `register_bridge_msg_receiver(int fd, EventCallback cb)` に置き換え、既存の `aux_mqs_` 同様に epoll へ登録する。コールバックは fd の `read()` ループを呼び出し、`read` の戻り値サイズでメッセージ型を判別する形に変更。

#### 5.8.3. `poll_for_unlink`

`mq_unlink(create_mq_name_for_bridge(PERFORMANCE_BRIDGE_VIRTUAL_PID))` を含むブロックを削除する (kmod 経由化された Bridge 向けメッセージに MQ ファイルは存在しない)。

### 5.9. AGNOCAST_BRIDGE_MODE = Off の挙動

`register_pubsub_bridge_core` / `register_service_bridge_core` の `if (bridge_mode == BridgeMode::On)` ガードはそのまま残し、`Off` 時は `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` を発行しない。kmod 側にも何も渡らないため、当該 IPC-ns で `bridge_msg_queue` が作られることもない。R4 を満たす。

## 6. リスクとトレードオフ (Risks / Tradeoffs)

| リスク | 影響 | 緩和策 |
| --- | --- | --- |
| kmod ABI に 1 メッセージ上限 (`MAX_BRIDGE_MSG_SIZE`) を持ち込む | 将来 Bridge メッセージがこの上限を超えるほど大きくなると kmod を minor bump して上限を広げる必要がある | `MAX_BRIDGE_MSG_SIZE = 2048` は現行の `MqMsgPerformanceBridge` の約 2 倍。Bridge メタデータの拡張に十分なリザーブ。kmod は中身を一切解釈しないため、上限以内のレイアウト変更は agnocastlib 単独で可能。 |
| receiver fd の `.read` 呼び出しスレッドの IPC-ns が想定と異なる可能性 | receiver fd は作成時の `q` ポインタを `file->private_data` に持つため、どのスレッドから `read` しても同じキューを参照する。IPC-ns を再取得しないので問題なし。 | ドキュメントで「receiver fd は作成プロセス内でのみ使用すること」を明記する。 |
| キュー上限で登録がドロップ | Bridge 終了後にキューをクリアしない (§5.6.3, §5.6.4) ため、Bridge が二度と起動しない運用では entity 作成のたびにキューが伸び続け、最終的に上限で押し出される | (a) 上限値 1024 で Autoware 規模を 1 桁カバー (b) 上限到達時は `dev_warn_ratelimited` で運用検知 (c) 現行 MQ も `MAX_MESSAGES=256` で先に頭打ちになるため本設計のほうが粗い意味でゆとりがある (d) Bridge 完全停止後も登録が来る挙動自体は `AGNOCAST_BRIDGE_MODE=off` で抑止可能 |
| receiver fd と `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` の同時アクセス競合 | 両者とも `global_htables_rwsem` を介して隔離される (write中は read がブロックされるためロストの不整合は起きない)。`copy_to_user` はロック外で行うため page fault をデータプレーンから隔離できる。 | 競合経路がないことを KUnit (`agnocast_kunit_bridge_msg_queue.c` 新設) でカバー |
| Bridge が receiver fd を作る前に最初の Agnocast プロセスが exit し、その後新しいプロセスが上がるシナリオ | その場合 `agnocast_process_exit_cleanup` (§5.6.6) でキューが一旦 kfree され、新しいプロセスが最初の `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` を出した時点で再生成される。 | Bridge も entity ももともと「最初のプロセスが起きるまで push されたエントリを受け取らない」セマンティックスであり、現状 MQ も同じ (誤差上のリスクとしては本設計で悪化していない)。 |
| ABI 後方互換 | 旧 user-space + 新 kmod の組み合わせ時、旧 user-space は `mq_send` 経路を使い、bridge_manager (新) は kmod 経路しか聞かないので登録が届かない | `lib` / `kmod` のバージョンチェック (`compare_to_minor_version` 既存) で minor bump を強制し、最低保証として両側を同時更新する。トランジション期間として「kmod は MQ 経路も並行サポート」する案も検討可能だが、本設計では minor bump によりカット。 |

## 7. 実装計画 (Implementation Plan)

1. **kmod (agnocast_kmod/)**
   - `agnocast.h` に `MAX_BRIDGE_MSG_SIZE`, `ioctl_send_msg_to_bridge_args`, `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` (28), `AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` (29) を追加。`MqMsgPerformanceBridge` のサイズ定数は含めない。
   - `agnocast_internal.h` / `agnocast_internal.c` に `bridge_msg_entry` (flexible array 付き), `bridge_msg_queue` 構造体・hashtable・helpers を追加。
   - `agnocast_ioctl.c` に `agnocast_ioctl_send_msg_to_bridge` (push) / `agnocast_ioctl_create_bridge_msg_receiver` (anonymous fd 生成) を実装。`size` フィールドのバリデーション (`0 < size <= MAX_BRIDGE_MSG_SIZE`) を含む。
   - `agnocast_ioctl.c` に `bridge_msg_receiver_fops` (`.read` / `.poll` / `.release`) を実装。「user buffer がメッセージより小さければ `-EMSGSIZE`」の datagram セマンティクスを採用。
   - `agnocast_ioctl_notify_bridge_shutdown` の変更なし（キュークリアは不要、既存フラグ操作のみ）。
   - `agnocast_process_exit_cleanup` (`agnocast_internal.c`) の末尾に `bridge_msg_queue` を IPC-ns スコープで kfree するブロックを 1 つ追加 (§5.6.6)。
   - `agnocast_exit_free_data` に `bridge_msg_queue_htable` の全削除 (safety net) を追加。
   - KUnit: `agnocast_kunit/agnocast_kunit_bridge_msg_queue.{c,h}` を新設し `agnocast_kunit_main.c` に登録。テスト項目: send·read の基本挙動、キュー上限、Bridge 不在中のバッファリング、`agnocast_process_exit_cleanup` での自動 kfree、可変長サイズのメッセージを正しく取り出せること。
2. **agnocastlib (src/agnocastlib/)**
   - `agnocast_mq.hpp` の `MqMsgPerformanceBridge` と `PERFORMANCE_BRIDGE_MQ_*` 定数は変更しない。`sizeof(MqMsgPerformanceBridge) <= MAX_BRIDGE_MSG_SIZE` の `static_assert` を追加するのみ。
   - `bridge/agnocast_bridge_node.hpp` の `send_mq_message` / `send_performance_*_bridge_registration*` を `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` ioctl 1 回に置換。`send_mq_message` テンプレート・リトライループは削除。
   - `bridge/agnocast_bridge_ipc_event_loop_base.hpp` の Primary MQ 廃止。`AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD` で receiver fd を取得し epoll に登録するセットアップを追加。
   - `bridge/performance/agnocast_performance_bridge_manager.cpp` の `on_mq_request` を `on_bridge_msgs` (`read()` ループ) にリネーム・実装変更。read の戻り値サイズで `MqMsgPerformanceBridge` をディスパッチ。
   - `agnocast.cpp` の `poll_for_unlink` から bridge MQ unlink を削除。
   - `test/unit/` に `sizeof(MqMsgPerformanceBridge) <= MAX_BRIDGE_MSG_SIZE` を検証する `test_bridge_msg_ioctl.cpp` を追加。
3. **docs**
   - `docs/message_queue.md` の "How message queue is used in Agnocast Bridge?" 節を「kmod 経由化により廃止」と書き換え、本 Design Doc へリンク。
   - 本ファイル `bridge-mq-removal.design-doc.md` をリポジトリに残す。
4. **トラブルシューティング (README)**
   - `mq_open failed: No space left on device` の項から Bridge MQ に関する記述を削除。`/agnocast_bridge_manager@*` のクリーンアップ手順 / `fs.mqueue.queues_max` 引き上げ案内は subscriber notification MQ 用に残置。
5. **Migration & Versioning**
   - kmod / agnocastlib / heaphook を minor bump (X.Y → X.(Y+1).0)。`compare_to_minor_version` チェックにより新旧混在運用は起動時にエラー。
   - `dkms.conf` の `version.txt` を更新。

## 8. 将来の拡張 (Future Work)

- **Daemon → bridge_manager の `MqMsgDaemonBridge` (`/agnocast_daemon_bridge_perf`) も同パターンで kmod 化可能**。本設計の `bridge_msg_queue` はメッセージの中身を一切解釈しないため、同じキューに複数種類のメッセージ (トピック登録 / daemon 起源のトピック設定 / 他) を混在させることも原理上は可能。Python 側 (`ros2agnocast_discovery_agent/bridge_decider.py`) は librt の `mq_open`/`mq_send` を ctypes で呼んでいるが、同じく `fcntl.ioctl()` で `AGNOCAST_SEND_MSG_TO_BRIDGE_CMD` を呼ぶ形に置換できる。スコープ拡大のため本 Design Doc の対象外。
- **Subscriber publish notification (`/agnocast@<topic>@<id>`) はホットパス**（メッセージ毎に 1 回 `mq_send` する）であり、kmod 経由化はオーバヘッドが許容できない可能性が高い。引き続き MQ を使う前提で、`poll_for_unlink` 経由のクリーンアップ精度向上を別軸で進める。
- 本 Design Doc の `bridge_msg_queue` は、Bridge ヘルスチェック結果のフィードバックなど、将来「Bridge とプロセス間で交換したい非ホットパスメッセージ」をそのまま乗せられる。kmod は中身を解釈せず `MAX_BRIDGE_MSG_SIZE` 以内という制約だけ課すため、新しいメッセージ型を追加したいときに kmod を minor bump しなくてよい。
