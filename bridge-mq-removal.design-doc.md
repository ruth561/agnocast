# Design Doc: Bridge登録MQのkmod常駐キューによる代替

> Status: Proposed
> Scope: kmod (minor-update-label) + agnocastlib + bridge_manager
> Related: [docs/message_queue.md#how-message-queue-is-used-in-agnocast-bridge](docs/message_queue.md)

## 1. 背景と目的 (Background & Objectives)

Agnocast Bridge は、Publisher/Subscriber/Service の生成タイミングで「Bridge を張ってほしい」というリクエストを Bridge Manager に通知している。現在この通知経路には POSIX Message Queue (`/agnocast_bridge_manager@-1[_d<ROS_DOMAIN_ID>]`) が使われており、エンティティ側が `mq_open(O_CREAT | O_WRONLY | O_NONBLOCK)` で MQ を作成して `MqMsgPerformanceBridge`（約 1.1 KB / トピック名・メッセージ型名・target_id・direction・is_service・サービス用 union を保持）を 1 回だけ送信する片方向通信である。

この構成には次の構造的な問題がある。

1. **MQ ファイルがクリーンアップされない**: 生成側 (Pub/Sub) は MQ を所有しないので unlink できない。所有者である Bridge Manager は自身の `IpcEventLoopBase::cleanup_resources()` で `mq_unlink` するが、SIGKILL や OOM など正常終了経路を通らない場合に `/dev/mqueue` 上にファイルが残り、再起動時の `fs.mqueue.queues_max` 上限を圧迫する事故が継続的に報告されている。
2. **`fs.mqueue.*` リソースに対する依存**: ホスト側 sysctl の値に挙動が左右され、CI / 実車間で再現性のないバグが生じる。
3. **Bridge 起動レースの存在**: Bridge デーモンは初回プロセス側で `fork()` 起動されるため、最初のエンティティが MQ を `O_CREAT` し終えた直後 (10 ms 〜 20 ms オーダー) に Bridge が `mq_open` で受信側を開ける。MQ がカーネル内バッファとして機能しているため致命的な取りこぼしは起きていないが、リソース管理を MQ に依存している以上、このレースの上に常に立っている。

本設計の目的は、`MqMsgPerformanceBridge` を運ぶ POSIX MQ を **kmod 常駐の per-IPC-namespace キュー** に置き換え、`mq_*` への依存を Bridge 経路から完全に切ることである。これにより、リソースは OS（モジュールアンロード）が必ず回収する形となり、`fs.mqueue.*` 由来のバグクラスが排除される。

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

最初の Agnocast プロセス内で、`fork()` 完了前にエンティティが生成された場合でも通知が失われないこと。Bridge は起動直後にバッファ済みリクエストを一括取得できる。

### R6. Bridge 終了後の挙動

現状の運用では同一 IPC-namespace 内で Bridge は再生成されないが、kmod 側にはその制約を焼き込まない。**Bridge 終了後に push されたリクエストはキューに保持され、次に Bridge が立ち上がったときに drain される**こと。これは MQ ベースの現行実装が `/dev/mqueue/` 上のファイルを介して結果的に提供している性質 (Bridge 不在期間中の `mq_send` がカーネル内バッファに蓄積され、次の `mq_open(O_RDONLY)` で読み出せる) を新設計でも維持する、という意味である。

### R7. kmod ABI 後方互換

既存 ioctl はすべて据え置く。新規 ioctl の追加のみで成立させ、minor バージョンアップに収める。

### R8. ロック衝突の最小化

通知は 1 process : 1 IPC-namespace 内で N 個の publisher/subscriber が同時に送る可能性がある (N producers / 1 consumer)。この経路がデータプレーンのロック (per-topic `topic_rwsem`, `global_htables_rwsem` の write 取得など) と干渉しないこと。

## 3. 既存資産の確認 (Existing Building Blocks)

新規実装で参照すべき先行パターンが kmod 内にすでに存在する。

| パターン | 既存実装 | 新設計での再利用 |
| --- | --- | --- |
| Per-IPC-namespace スコープの bridge トラッキング | `bridge_htable` / `agnocast_ioctl_add_bridge` / `is_performance_bridge_manager` flag in `process_info` | 「現在 alive な bridge_manager が誰か」を判定する既存ソース |
| 2-phase get/commit ioctl で kmod のキューを drain する | `AGNOCAST_GET_EXIT_PROCESS_CMD` / `agnocast_commit_exit_process` (ロックを保持したまま `copy_to_user` しない) | 本設計の drain ioctl はこのパターンを踏襲 |
| Process exit 時の kmod 内クリーンアップ kthread | `pre_handler_subscriber_exit` / `agnocast_process_exit_cleanup` / `worker_wait` waitqueue | bridge_manager 異常終了時のキューリセットに利用 |
| データプレーンと隔離した独立ロック | `pid_queue_lock` (PID exit 用 ring buffer) | キュー専用の spinlock を切ることで `global_htables_rwsem` 取得を回避 |

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

新規 ioctl を 2 本 (`PUSH` / `DRAIN`) と `.poll` 実装を `agnocast` キャラクタデバイスに追加し、`MqMsgPerformanceBridge` 相当のペイロードを per-IPC-namespace の FIFO キューに格納する。

- Pros:
  - R1 を完全に満たす（カーネル内メモリのみ。モジュールアンロードで全解放される）。
  - R2 を満たす（push は spinlock + list_add_tail で O(1)、Bridge 不在でもキューに溜まる）。
  - R5 を満たす（Bridge 起動時にまとめて drain）。
  - 既存の `bridge_htable` / `is_performance_bridge_manager` 状態と同じ kmod 内で完結し、Bridge 終了の検知 (§6.4) を流用できる。
  - 通知に `eventfd` ではなく **既存の `agnocast_fd` の `.poll`** を使うことで、Bridge Manager 側の `epoll` ループに新たな fd を増やさない（`IpcEventLoopBase` の `register_aux_mq` を 1 つの `add_fd_to_epoll(agnocast_fd_, "BridgeRequest")` に置き換えるだけで済む）。
- Cons:
  - kmod ABI が増える（minor bump）。設計ミスを後から直しにくい。
  - kmod 側に「Bridge 用の薄いメッセージブローカー」が常駐することになる。
- 結論: **採用**。kmod は既に bridge_htable と `is_performance_bridge_manager` で「Bridge とは何者か」を知っており、本キューはその責務の自然な拡張である。共有メモリ＋通知やソケットでは取れない「OS による絶対的なリソース回収」が要件 R1 から不可避である。

### Bridge 期間モデルの選定

ドラフト中で 3 つの選択肢が議論されていた。

| モデル | 内容 | 採用可否 |
| --- | --- | --- |
| (a) Bridge 存在中に作成された entity のみ | Bridge 起動を待ってから push する | R2 違反 |
| (b) Bridge 起動以前の entity も含む | Bridge 不在でも push、Bridge は起動時にバッファを drain | **採用** |
| (c) Bridge 登録時に kmod から現存 entity の snapshot を取る | push を廃止し、`AGNOCAST_GET_TOPIC_*_INFO` 系を拡張 | 却下: 既存 ioctl 群はメッセージ型名やサービス用 shadow node 情報を保持しておらず、これらを kmod に格納すると Bridge 固有のメタを kmod に持ち込むことになる。責務が滲む。 |

(b) を採用することで、ペイロードと所有者がそれぞれ「user-space で完結する Bridge 固有メタ」「kmod が責務を持つ FIFO の物理的存在」に綺麗に分離できる。

## 5. 詳細設計 (Proposed Architecture)

### 5.1. 新規 ioctl

| 番号 | 名前 | 方向 | 用途 |
| --- | --- | --- | --- |
| `_IOW(0xA6, 28, ...)` | `AGNOCAST_PUSH_BRIDGE_REQUEST_CMD` | producer → kmod | エンティティ作成時、現行の `send_mq_message` の代替。1 件の `MqMsgPerformanceBridge` 相当ペイロードをキュー末尾へ追加する。 |
| `_IOWR(0xA6, 29, ...)` | `AGNOCAST_DRAIN_BRIDGE_REQUESTS_CMD` | bridge_manager ← kmod | キューの先頭から最大 N 件を user-space バッファへコピーし、コピー成功後に kmod 側で削除する。`ret_call_again` で残件を伝える。 |

`AGNOCAST_NOTIFY_BRIDGE_SHUTDOWN_CMD` (既存) と process exit fastpath (既存 `agnocast_process_exit_cleanup`) の 2 つは bridge_manager 終了の検知点として再利用する。新規の終了用 ioctl は追加しない。

### 5.2. ペイロード型

`agnocast.h` に kmod 側の正式な型を 1 つだけ定義し、user-space (`agnocast_mq.hpp` の `MqMsgPerformanceBridge`) は型エイリアス（または等価な POD 構造体）として参照する。

```c
/* agnocast.h */
#define BRIDGE_MESSAGE_TYPE_BUFFER_SIZE 256
#define BRIDGE_SERVICE_NAME_BUFFER_SIZE 256
#define BRIDGE_SERVICE_TYPE_BUFFER_SIZE 256

struct ioctl_pubsub_bridge_target
{
  char message_type[BRIDGE_MESSAGE_TYPE_BUFFER_SIZE];
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  topic_local_id_t target_id;
};

struct ioctl_service_bridge_target
{
  char service_type[BRIDGE_SERVICE_TYPE_BUFFER_SIZE];
  char service_name[BRIDGE_SERVICE_NAME_BUFFER_SIZE];
  bool create_shadow_node;
  char shadow_node_namespace[NODE_NAME_BUFFER_SIZE];
  char shadow_node_name[NODE_NAME_BUFFER_SIZE];
};

struct ioctl_bridge_request
{
  union {
    struct ioctl_pubsub_bridge_target pubsub_target;
    struct ioctl_service_bridge_target srv_target;
  };
  uint32_t direction;   /* BridgeDirection */
  bool is_service;
};
```

`MqMsgPerformanceBridge` は `using MqMsgPerformanceBridge = ioctl_bridge_request;` 相当に移行する。両者は同一翻訳単位 ABI 上で同じレイアウトを取る POD であり、サイズ・オフセットを保証する既存 unit test (`src/agnocastlib/test/unit/test_daemon_bridge_mq.cpp` と同種のもの) を新設する。

#### push args / drain args

```c
struct ioctl_push_bridge_request_args
{
  struct ioctl_bridge_request request;
};

#define MAX_BRIDGE_REQUEST_DRAIN_NUM 16  /* ヒューリスティック値, MAX_RECEIVE_NUM と整合 */

struct ioctl_drain_bridge_requests_args
{
  /* input */
  uint64_t request_buffer_addr;     /* user buffer (struct ioctl_bridge_request[]) */
  uint32_t request_buffer_size;     /* element count */
  /* output */
  uint32_t ret_drained_num;
  bool ret_call_again;              /* キューにまだ残っているか */
};
```

`ret_pub_shm_info` 系と同じく、固定長の小さなペイロードは引数構造体に入れ、可変長部分は user buffer へ `copy_to_user` する 2 段構成。

### 5.3. kmod 内部データ構造

```c
struct bridge_request_entry
{
  struct ioctl_bridge_request payload;
  struct list_head node;
};

struct bridge_request_queue
{
  const struct ipc_namespace * ipc_ns;
  struct list_head entries;        /* FIFO, push: tail / drain: head */
  uint32_t entry_count;
  spinlock_t lock;                 /* producers と consumer の隔離用 */
  wait_queue_head_t wait;          /* .poll が poll_wait する */
  struct hlist_node hnode;
};

DECLARE_HASHTABLE(bridge_request_queue_htable, 6);  /* IPC-ns 数は通常 1 〜 数個 */
```

- 1 IPC-namespace につき 1 個。最初の push が来た時点で lazy に作成。
- 既存 `bridge_htable` とは別の構造（`bridge_htable` は per-topic、こちらは per-IPC-ns）。
- 専用 `spinlock_t lock` を持ち、データプレーンの `global_htables_rwsem` / `topic_rwsem` を取らない。

### 5.4. キュー上限と溢れ時の動作

```c
#define MAX_BRIDGE_REQUEST_QUEUE_SIZE 1024  /* per IPC-namespace */
```

- 1 件 ~1.1 KB → 上限到達時 ~1.1 MB / IPC-ns。
- 上限に達した状態で push が来た場合は **エンキューせず `-ENOSPC` を返す**。`dev_warn_ratelimited` で警告。
- これは現行の `mq_send` が EAGAIN を返した場合の user-space 側挙動 (100 回 × 100 ms リトライ後にエラーログ) と等価か、それより安全なフォールバック挙動 (リトライしないので push のレイテンシが青天井にならない)。

`MAX_BRIDGE_REQUEST_QUEUE_SIZE` は現行の `PERFORMANCE_BRIDGE_MQ_MAX_MESSAGES = 256` を 4 倍程度にした安全側の値。Autoware 規模 (~数百ノード) で、Bridge 起動前バッファとして十分なヘッドルームを確保する。

### 5.5. 通知機構: `.poll` の追加

`agnocast` キャラクタデバイスの `file_operations` に `.poll` を追加する。

```c
static unsigned int agnocast_poll(struct file * file, poll_table * wait)
{
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  struct bridge_request_queue * q;

  rcu_read_lock();
  q = find_bridge_request_queue_rcu(ipc_ns);
  rcu_read_unlock();

  if (!q) return 0;

  poll_wait(file, &q->wait, wait);

  /* Lock-free fast path: list_empty() の確認だけ。実際の取り出しは drain ioctl で行う。 */
  return list_empty(&q->entries) ? 0 : (POLLIN | POLLRDNORM);
}

static const struct file_operations fops = {
  .owner          = THIS_MODULE,
  .unlocked_ioctl = agnocast_ioctl,
  .poll           = agnocast_poll,
};
```

push 側は `list_add_tail` 直後に `wake_up_interruptible(&q->wait)` を呼び、Bridge Manager の `epoll_wait` を起こす。

#### Bridge Manager 側

`PerformanceBridgeIpcEventLoop` の Primary MQ (`create_mq_name_for_bridge`) を**廃止**し、代わりに `agnocast_fd` を `epoll` に登録する。

```cpp
// Before
event_loop_.set_mq_handler([this](int fd) { this->on_mq_request(fd); });

// After
event_loop_.set_bridge_request_handler([this]() { this->on_bridge_requests(); });
// IpcEventLoopBase は agnocast_fd を epoll(EPOLLIN) で監視するよう改修。
```

`on_bridge_requests()` は `AGNOCAST_DRAIN_BRIDGE_REQUESTS_CMD` を `ret_call_again == false` になるまで呼び出すループ (現行の `on_daemon_mq_request` と同形)。

> Note: `agnocast_fd` を直接 epoll する以上、`fcntl(agnocast_fd, F_SETFL, O_NONBLOCK)` 相当は不要 (poll は read を行わないため)。drain ioctl 自体は固有のループバックを持つ。

### 5.6. ライフサイクル

#### 5.6.1. キュー生成

最初の `AGNOCAST_PUSH_BRIDGE_REQUEST_CMD` を受けた時点で、対応する IPC-ns の `bridge_request_queue` を kmalloc し `bridge_request_queue_htable` に登録する。

#### 5.6.2. Bridge Manager 起動シーケンス

1. `acquire_agnocast_resources_for_bridge()` 内で `AGNOCAST_ADD_PROCESS_CMD(is_performance_bridge_manager=true)` を発行 (既存)。
2. `IpcEventLoopBase` 構築時に `agnocast_fd` を epoll に登録。
3. `spin_once` の最初の呼び出しでバッファ済みリクエストを drain し、現行 `on_mq_request` と同等のロジックでブリッジを生成。

R5 の「Bridge 起動以前の entity も含む」要件はこれで満たされる (push は kmod 内のキューにすでに溜まっているため)。

#### 5.6.3. Bridge Manager 正常終了

`AGNOCAST_NOTIFY_BRIDGE_SHUTDOWN_CMD` は **キューをクリアしない**。`process_info->is_performance_bridge_manager` を `false` に倒すだけ (既存挙動)。`bridge_request_queue` 構造体・蓄積中のエントリは保持され、以降に push されたリクエストとあわせて、次に同 IPC-ns で `AGNOCAST_ADD_PROCESS_CMD(is_performance_bridge_manager=true)` を成功させた Bridge が drain することになる。

> 現状の運用では同 IPC-ns に Bridge は再生成されない (R6) が、kmod ABI として「Bridge は再起動可能」「Bridge 不在期間中の push も次の Bridge で受け取れる」を保証しておくことで、将来の運用変更（例: Bridge crash 時の自動再 fork、診断ツールが一時的に Bridge を spawn して内部状態を覗くワークフロー）を妨げない。

#### 5.6.4. Bridge Manager 異常終了

`agnocast_process_exit_cleanup` 経由の既存パスで `process_info->is_performance_bridge_manager == true` だった場合も同様に **キューをクリアしない**。`is_performance_bridge_manager` フラグだけを倒し、エントリ・キュー構造体は保持する。

> Bridge が死亡した瞬間、キューに残っていたエントリは「死亡した Bridge が drain しきれなかった分」である。新しい Bridge から見れば「Bridge 不在期間中の push」と区別する意味はないため、まとめて次の Bridge へ渡すのが自然。Bridge 側にはすでに `check_and_remove_request_cache` / `remove_invalid_requests` による dead-target 検知 (`get_subscriber_qos`/`get_publisher_qos` ioctl をプローブにして死んだ entity を弾く) が実装済みなので、古いリクエストが新 Bridge を誤動作させる懸念はない。

#### 5.6.5. キュー上限とBridge 不在期間中の保護

Bridge 終了後にキューがクリアされないため、「Bridge が二度と起動しない」運用 (現状の Autoware 動作) では、push が来るたびにキューが伸び続け、最終的に §5.4 の `MAX_BRIDGE_REQUEST_QUEUE_SIZE = 1024` で頭打ちになる。上限到達後の push は `-ENOSPC` で静かに落とされる (運用上の検知性は `dev_warn_ratelimited`)。これは現状の MQ 実装が `MAX_MESSAGES = 256` で先に頭打ちになる挙動と等価で、本設計が悪化させるシナリオではない。

#### 5.6.6. キュー破棄

`bridge_request_queue` 構造体および蓄積エントリは **モジュールアンロードまで kmod 内に残る**（IPC-ns ごとに 1 個 + 上限 1024 エントリ × ~1.1 KB ≒ 1.1 MB の最悪値）。`agnocast_exit_free_data` で hashtable をループ削除し、エントリ・キュー構造体ともに `kfree` する。R1 を満たす最終的なリソース回収点。

### 5.7. ロック設計

- producer の hot path (`AGNOCAST_PUSH_BRIDGE_REQUEST_CMD`):
  1. `find_bridge_request_queue_rcu` (RCU read-side, 既存 hashtable パターン)
  2. 無ければ `down_write(&bridge_request_queue_rwsem)` で作成 (cold path、最初の 1 回のみ)
  3. `spin_lock(&q->lock)` → list_add_tail → カウンタ増 → `spin_unlock`
  4. `wake_up_interruptible(&q->wait)`
- consumer の drain path:
  1. `spin_lock(&q->lock)` → リストから N 件を**ローカルリストへ移動** → `entry_count` 減 → `spin_unlock`
  2. (lock-free) ローカルリストから kmalloc 済みのエントリを `copy_to_user` でユーザーバッファへ転写
  3. `kfree` でエントリ解放

ステップ 2 の `copy_to_user` を spinlock 外で行うことが重要 (page fault による無制限のレイテンシをデータプレーンから隔離)。これは `agnocast_ioctl_get_exit_process` の 2-phase 設計と完全に同型。

### 5.8. user-space 側の差分

#### 5.8.1. `agnocast_publisher.cpp` / `agnocast_subscription.cpp` / 各種 service 生成パス

`send_mq_message(...)` 経由の `mq_open(O_CREAT|O_WRONLY|O_NONBLOCK)` → `mq_send` → `mq_close` のシーケンスを `ioctl(agnocast_fd, AGNOCAST_PUSH_BRIDGE_REQUEST_CMD, &args)` の 1 回呼び出しに置き換える。

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

  struct ioctl_push_bridge_request_args args = {};
  static_assert(sizeof(args.request) == sizeof(msg), "ABI mismatch");
  std::memcpy(&args.request, &msg, sizeof(msg));

  if (ioctl(agnocast_fd, AGNOCAST_PUSH_BRIDGE_REQUEST_CMD, &args) < 0) {
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
- 代わりに `agnocast_fd` を epoll に登録するセットアップを追加。
- `set_mq_handler(EventCallback)` のシグネチャは維持しつつ実装を `bridge_request_handler` に置き換えるか、新規に `set_bridge_request_handler(SocketlessCallback)` を追加して旧 API を deprecate。

#### 5.8.3. `poll_for_unlink`

`mq_unlink(create_mq_name_for_bridge(PERFORMANCE_BRIDGE_VIRTUAL_PID))` を含むブロックを削除する (kmod 経由化された Bridge 通知に MQ ファイルは存在しない)。

### 5.9. AGNOCAST_BRIDGE_MODE = Off の挙動

`register_pubsub_bridge_core` / `register_service_bridge_core` の `if (bridge_mode == BridgeMode::On)` ガードはそのまま残し、`Off` 時は `AGNOCAST_PUSH_BRIDGE_REQUEST_CMD` を発行しない。kmod 側にも何も渡らないため、当該 IPC-ns で `bridge_request_queue` が作られることもない。R4 を満たす。

## 6. リスクとトレードオフ (Risks / Tradeoffs)

| リスク | 影響 | 緩和策 |
| --- | --- | --- |
| kmod ABI に Bridge 固有のペイロード (`message_type`, shadow node 情報) を持ち込む | 将来 Bridge 仕様が変わるたびに kmod を更新する必要が生じる可能性 | 構造体は完全に opaque (`char[]` バッファ + 数フィールド)。kmod は中身をパースしない。仕様追加時はバッファサイズを拡張する minor bump で済む。 |
| .poll が `current->nsproxy->ipc_ns` に依存する | Bridge Manager が別スレッドから epoll する場合に NS が一致しない懸念 | Bridge Manager のメインスレッドのみが epoll を回す現行設計 (`agnocast_performance_bridge_ipc_event_loop.cpp`) を維持する。NS 越えに使うことは documentation で禁止。 |
| キュー上限で push がドロップ | Bridge 終了後にキューをクリアしない (§5.6.3, §5.6.4) ため、Bridge が二度と起動しない運用では entity 作成のたびにキューが伸び続け、最終的に上限で押し出される | (a) 上限値 1024 で Autoware 規模を 1 桁カバー (b) 上限到達時は `dev_warn_ratelimited` で運用検知 (c) 現行 MQ も `MAX_MESSAGES=256` で先に頭打ちになるため本設計のほうが粗い意味でゆとりがある (d) Bridge 完全停止後も push を続ける挙動自体は `AGNOCAST_BRIDGE_MODE=off` で抑止可能 |
| `agnocast_fd` を epoll に追加することで他のスレッドからの `ioctl` と相互作用しないか | `.poll` は file lock を取らずに list_empty を見るだけ、`ioctl` は queue spinlock を取るため相互排他は確保される | 競合経路がないことを KUnit (`agnocast_kunit_bridge_request_queue.c` 新設) でカバー |
| ABI 後方互換 | 旧 user-space + 新 kmod の組み合わせ時、旧 user-space は `mq_send` 経路を使い、bridge_manager (新) は kmod 経路しか聞かないので通知が届かない | `lib` / `kmod` のバージョンチェック (`compare_to_minor_version` 既存) で minor bump を強制し、最低保証として両側を同時更新する。トランジション期間として「kmod は MQ 経路も並行サポート」する案も検討可能だが、本設計では minor bump によりカット。 |

## 7. 実装計画 (Implementation Plan)

1. **kmod (agnocast_kmod/)**
   - `agnocast.h` に `ioctl_bridge_request`, `ioctl_push_bridge_request_args`, `ioctl_drain_bridge_requests_args`, 2 つの ioctl 番号 (28, 29) を追加。
   - `agnocast_internal.h` / `agnocast_internal.c` に `bridge_request_queue` 構造体・hashtable・helpers を追加。
   - `agnocast_ioctl.c` に `agnocast_ioctl_push_bridge_request` / `agnocast_ioctl_drain_bridge_requests` を実装。
   - `agnocast_init.c` の `fops` に `.poll = agnocast_poll` を追加。
   - `agnocast_ioctl_notify_bridge_shutdown` / `agnocast_process_exit_cleanup` にキュークリア処理を追加。
   - `agnocast_exit_free_data` に hashtable 全削除を追加。
   - KUnit: `agnocast_kunit/agnocast_kunit_bridge_request_queue.{c,h}` を新設し `agnocast_kunit_main.c` に登録。
2. **agnocastlib (src/agnocastlib/)**
   - `agnocast_mq.hpp` の `MqMsgPerformanceBridge` を `ioctl_bridge_request` のエイリアス化、`PERFORMANCE_BRIDGE_MQ_*` 定数を削除。
   - `bridge/agnocast_bridge_node.hpp` の `send_mq_message` / `send_performance_*_bridge_registration*` を ioctl 呼び出しに置換。
   - `bridge/agnocast_bridge_ipc_event_loop_base.hpp` の Primary MQ 廃止、`agnocast_fd` epoll 登録追加。
   - `bridge/performance/agnocast_performance_bridge_manager.cpp` の `on_mq_request` を `on_bridge_requests` (drain ループ) にリネーム・実装変更。
   - `agnocast.cpp` の `poll_for_unlink` から bridge MQ unlink を削除。
   - 既存の `test/unit/test_daemon_bridge_mq.cpp` と同様の ABI サイズテストを `test_bridge_request_ioctl.cpp` として追加。
3. **docs**
   - `docs/message_queue.md` の "How message queue is used in Agnocast Bridge?" 節を「kmod 経由化により廃止」と書き換え、本 Design Doc へリンク。
   - 本ファイル `bridge-mq-removal.design-doc.md` をリポジトリに残す。
4. **トラブルシューティング (README)**
   - `mq_open failed: No space left on device` の項から Bridge MQ に関する記述を削除。`/agnocast_bridge_manager@*` のクリーンアップ手順 / `fs.mqueue.queues_max` 引き上げ案内は subscriber notification MQ 用に残置。
5. **Migration & Versioning**
   - kmod / agnocastlib / heaphook を minor bump (X.Y → X.(Y+1).0)。`compare_to_minor_version` チェックにより新旧混在運用は起動時にエラー。
   - `dkms.conf` の `version.txt` を更新。

## 8. 将来の拡張 (Future Work)

- **Daemon → bridge_manager の `MqMsgDaemonBridge` (`/agnocast_daemon_bridge_perf`) も同パターンで kmod 化可能**。Python 側 (`ros2agnocast_discovery_agent/bridge_decider.py`) は librt の `mq_open`/`mq_send` を ctypes で呼んでいるが、同じく `fcntl.ioctl()` で `AGNOCAST_PUSH_DAEMON_BRIDGE_REQUEST_CMD` (仮) を呼ぶ形に置換できる。本設計の `bridge_request_queue` をテンプレ化して 2 種のキュー (entity origin / daemon origin) に対応させればよい。スコープ拡大のため本 Design Doc の対象外。
- **Subscriber publish notification (`/agnocast@<topic>@<id>`) はホットパス**（メッセージ毎に 1 回 `mq_send` する）であり、kmod 経由化はオーバヘッドが許容できない可能性が高い。引き続き MQ を使う前提で、`poll_for_unlink` 経由のクリーンアップ精度向上を別軸で進める。
- 本 Design Doc の `bridge_request_queue` は将来的に Bridge 以外の **「kmod が代理する非ホットパス通知」全般** に流用しうる。Bridge ヘルスチェック結果のフィードバック等を運ぶ汎用キューに抽象化する余地がある。
