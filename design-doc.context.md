# [進行中] agnocast pub/sub による ECU 間通信

**Jira:** [T4DEV-51976](https://tier4.atlassian.net/browse/T4DEV-51976) — agnocast pub/sub による ECU 間通信
**親 Epic:** [T4DEV-52095](https://tier4.atlassian.net/browse/T4DEV-52095) — Agnocast per-IPC デーモンプロセスの拡張による multi-IPC namespace 対応の強化
**ベースライン:** IMAI 叩き台 [https://tier4.atlassian.net/wiki/x/fIDcMwE](https://tier4.atlassian.net/wiki/x/fIDcMwE) (F1 部分 / 共通基盤を参照、本 doc は具体化と差分提案を扱う。差分まとめは §7 Appendix)
**関連設計:** F3 観測性 ([https://tier4.atlassian.net/wiki/x/VAGzNgE](https://tier4.atlassian.net/wiki/x/VAGzNgE)) — 共通基盤 (per-IPC daemon, Gossip protocol) を共有
**Reviewers:** Koichi IMAI
**Status:** In Review

## 1. TL;DR

異なる IPC namespace / 異なる ECU に分散した agnocast pub と sub の間で、必要に応じて
**a2r / r2a bridge を on-demand に自動生成**する。Data Plane は既存の a2r / r2a
bridge ノードに任せ、ECU 間 / NS 間は ROS 2 DDS が中継する。

per-IPC daemon (新パッケージ `ros2agnocast_discovery_agent`、IPC namespace ごとに
1 つ起動する Python rclpy 別プロセス) が:

1. 自 NS の kmod 状態 (既存 NS-scoped ioctl 経由) と他 NS の状態 (DDS gossip) を
   突合して **bridge 要否を判定**
2. 必要分を **新 MQ + 新 msg type** で自 NS の bridge_manager に依頼

`need-patch-update` で完結 (kmod / agnocastlib / heaphook の既存 ABI を壊さない)。
F3 観測性タスクと **同じ daemon と Gossip protocol を共有**し、本 doc 側で daemon /
Gossip protocol の詳細を扱う (= shared base owner)。

## 2. 背景とスコープ

### 2.1 問題と通信ランドスケープ

**現状の課題**: 既存 bridge_manager は kmod ioctl 経由で自 IPC namespace 内の
agnocast pub/sub と ROS 2 pub/sub の存在を確認し、bridge の両端 (agnocast 側と
ROS 2 側) が同 NS に揃っていれば on-demand に a2r / r2a bridge を生成する。
しかし agnocast pub が NS-A、agnocast sub が NS-B に分散していると、両側の
bridge_manager は「ブリッジ相手となる ROS 2 pub/sub が自 NS に存在しない」と
判断し、bridge が一つも立たない。

ROS 2 自身は **net namespace が共有されていれば** DDS で互いを発見・通信できるので、
**IPC namespace 境界に bridge を 1 つ置けば中継できる**。この機構が欠けているのが
F1 の課題。

**通信ランドスケープ** (ECU / IPC NS / net NS / domain の 4 軸):

| ECU | IPC NS | net NS | domain | 通信 | F1 が解決? |
| --- | --- | --- | --- | --- | --- |
| same | same | same | same | ✅ 既存 a2a (共有メモリ直結) | 不要 |
| same | same | same | diff | ✅ kmod の domain 対応 | F2 領域 |
| **same** | **diff** | **same** | **same** | **✅ DDS 越し中継** | **対応** |
| same | diff | diff | * | ❌ ROS 2 でも届かない (同 kernel 内 net NS 分離は DDS の UDP を遮断) | scope 外 |
| **diff** | **diff** | **diff (※)** | **same** | **✅ DDS 越し中継** | **対応** |
| diff | diff | diff (※) | diff | ✅ + domain_bridge | F1 + F2 |

(※) 別 ECU は必然的に別 net NS だが、物理 network (IP routing) 経由で DDS RTPS が
到達する。同一 ECU + net NS 分離 (4 行目) とは状況が異なる。

F1 の本丸は **太字 2 行** — 同 ECU 上で IPC NS だけ分離している場合 (`--network=host --ipc=private` 等の構成) と、別 ECU 上 (分散 ROS 2) の場合。両者とも判定ロジックは
同じ:「両側に endpoint があり、DDS で互いを発見できるなら bridge を立てる」。

> **Note**: IMAI 叩き台 §3.1 では IPC NS と net NS を 1 軸に圧縮していたが、本 doc では
> 独立軸として扱い、上記 3 行目 (same ECU, diff IPC NS, same net NS) を一級ケースに
> 含める (Imai-san 2026-05-18 確認済み)。

### 2.2 ゴール / ノンゴール

**ゴール:**

1. NS-A の agnocast pub と NS-B の agnocast sub が同一 topic 名を持つとき、
   **NS-A に a2r bridge**、**NS-B に r2a bridge** を on-demand 自動生成
2. 別 ECU 間 / 同 ECU + 別 IPC NS (net NS 共有) の両ケースで動作
3. on-demand 維持 (両側に endpoint がある時のみ bridge を立てる、不要 bridge を
   作らない)
4. 片方の endpoint が終了したら bridge を片付ける
5. Standard / Performance 両モードで動作
6. `need-patch-update` で完結

**ノンゴール:**

- cross-NS の zero-copy Data Plane — そもそも Epic 要件外 (DDS 経由でよい)
- domain bridge / domain 分離 — F2 ([T4DEV-51894](https://tier4.atlassian.net/browse/T4DEV-51894) / [T4DEV-51895](https://tier4.atlassian.net/browse/T4DEV-51895)) の担当
- net NS も分離されたケース — ROS 2 でも届かない、隔離維持のために対象外
- 型解決を要する `topic hz / delay / pub / echo` 等の CLI — 別議論

## 3. 設計概要

### 3.1 全体構成

```
============================ Data Plane (既存、変更なし) ============================

+-- IPC NS-A ----------------+      +-- IPC NS-B ----------------+
| agnocast pub               |      | agnocast sub               |
+----------+-----------------+      +-----+----------------------+
|                              ^
v                              |
+-- a2r bridge node ---------+      +-- r2a bridge node ---------+ <-- bridge_manager
+----------+-----------------+      +-----+----------------------+      が on-demand 生成
v DDS                          ^ DDS
+------- ROS 2 DDS routing ----+
(net NS が同じ or ECU 越しに到達可能)

============================ Control Plane (本タスクで新規) =========================

+-- per-IPC daemon (NS-A) ---+      +-- per-IPC daemon (NS-B) ---+
| ros2agnocast_              |      | ros2agnocast_              |
|  discovery_agent (Python)  |      |  discovery_agent (Python)  |
| - ioctl で自 NS 状態取得   |      | - ioctl で自 NS 状態取得   |
| - gossip pub/sub           |      | - gossip pub/sub           |
| - bridge 要否判定          |      | - bridge 要否判定          |
| - 新 MQ で bridge 要求     |      | - 新 MQ で bridge 要求     |
+----------+-----------------+      +-----+----------------------+
|                              |
v 新 MQ                        v 新 MQ
+-- bridge_manager (NS-A) ---+      +-- bridge_manager (NS-B) ---+
| daemon 要求を listen して  |      | daemon 要求を listen して  |
| a2r / r2a node を生成      |      | a2r / r2a node を生成      |
+----------------------------+      +----------------------------+
```


Data Plane は既存 a2r / r2a bridge と DDS をそのまま使う。新規部分は **per-IPC
daemon の Control Plane**: 観測 (gossip) と指示 (新 MQ) を担う。

### 3.2 per-IPC daemon — shared base

**実装**: 新パッケージ `ros2agnocast_discovery_agent` (Python rclpy node)。
1 IPC namespace につき 1 プロセスを起動する。起動責任は user
(systemd unit / `ros2 launch` / container entrypoint 等)。

daemon 内タスク:

1. `state poller`: 既存 NS-scoped ioctl (`AGNOCAST_GET_TOPIC_LIST_CMD` 等) で
   自 NS の publisher / subscriber 状態を 1 Hz で poll (変化検知時は即実行)。
   ioctl は元々 caller の IPC NS に filter されているので、daemon は per-IPC NS で
   起動しているだけで自 NS のみ取得できる (cross-NS view は不要)
2. `gossip publisher`: 自 NS の状態を `AgnocastDaemonState` に詰めて
   `/_agnocast_discovery` に publish (1 msg = 1 NS)
3. `gossip subscriber + bridge decider`: `/_agnocast_discovery` を subscribe
   して他 NS の状態を取得、自 NS 状態と突合して bridge 要否を判定
4. `bridge requester`: 必要な bridge を新 MQ 経由で自 NS の bridge_manager に
   依頼 (`posix_ipc` 等で MQ を叩く、msg は §3.5)

**この設計を採った理由 (実装・レビュー surface の最小化)**:

- agnocastlib (C++) に rclcpp 依存を持ち込まない → C++ 改修 surface ゼロ
- 新規追加範囲を新パッケージ内に閉じ込められる (agnocastlib・kmod 非依存)
- 各 daemon が自 IPC NS にだけ責任を持つので NS scope の混乱がない

別案 (agnocastlib `poll_for_unlink` を C++ 拡張) は §6 で比較する。

### 3.3 Gossip protocol — shared base

**Topic**: `/_agnocast_discovery` (hidden topic、`_` prefix で ros2 標準 CLI からは
通常 hidden)

**QoS**: Reliable + Transient Local + Liveliness Automatic (30 秒 lease) + history
depth は per-NS の最新を 1 つ保持する (1 daemon が 1 NS 分の msg を publish するので
上書き衝突は発生しない)

**Schema** (`ros2agnocast_discovery_msgs` パッケージで定義、field 並びの詳細は msg
ファイル参照):

| msg | 主な field | 用途 |
| --- | --- | --- |
| `AgnocastDaemonState` | `schema_version`, `agnocast_version`, `host_uuid`, `host_hostname`, `timestamp`, `ipc_ns_inode`, `topics[]` | 1 daemon = 1 NS = 1 msg。stale 検知に `timestamp` |
| `AgnocastTopic` | `topic_name`, `type_name`, `domain_id`, `publishers[]`, `subscribers[]` | topic 1 件 |
| `AgnocastEndpoint` | `node_name`, `pid`, `qos_depth`, `qos_is_transient_local`, `qos_is_reliable`, `is_bridge` | pub または sub 1 件 |

`schema_version` を持って互換破壊版で bump (semver 的な細分化 — major/minor/patch
や extensions 領域での非破壊拡張ポリシー — は scope 外、将来 schema 拡張が頻発する
兆候が出てから検討)。`agnocast_version` は schema_version で原理上 cover できるが
debug / 互換性確認の冗長保険として併載する。`AgnocastTopic` に `type_name` を
含めて `ros2 topic info_agnocast` の型表示に使う。`is_bridge=true` の endpoint は
DDS で既に可視なので、CLI / daemon の merge 時に dedupe する。

**Publish 戦略**: 変化検知時の即 publish を主、heartbeat (0.1 Hz) を副。

### 3.4 bridge 要否判定 (high-level)

**入力**:

- 自 NS の kmod 状態 = 既存 ioctl で取得 (publisher / subscriber list)
- 他 NS の状態 = `/_agnocast_discovery` の subscribe 結果

**判定**: 自 NS の各 endpoint (pub or sub) について、他 NS に **同一 topic name** で
**逆方向の endpoint** が存在し、かつ自 NS にまだ対応する bridge が立っていなければ
bridge を要求する。

- 自 NS に agnocast pub があって、他 NS に agnocast sub (or ROS 2 sub) があれば
  → 自 NS に **a2r bridge** を要求
- 自 NS に agnocast sub があって、他 NS に agnocast pub (or ROS 2 pub) があれば
  → 自 NS に **r2a bridge** を要求

両側の daemon が独立にこの判定を走らせるので、自然に NS-A 側で a2r、NS-B 側で r2a が
立ち、DDS が結ぶ。

### 3.5 bridge_manager への指示

**新 MQ + 新 msg type** `MqMsgDaemonBridge` を導入する (既存 `MqMsgBridge` /
`MqMsgPerformanceBridge` は不変)。msg は型名ベースで daemon が埋められる
(`topic_name`, `type_name`, `direction`)。bridge_manager は新 MQ を追加 listen する。

bridge 生成時の factory 解決は両モードで非対称:

- **Performance mode**: 既存の plugin loader (`libbridge_plugin_<TYPE>.so` の固定
  シンボル) をそのまま再利用
- **Standard mode**: 既存仕様と本タスク追加の組合せで factory を解決する:
  - **既存仕様**: Standard bridge_manager は user process 内に同居し、bridge node も
    その process 内に立つ。`Publisher<T>` を使った user process には、bridge を作る
    factory 関数 (`start_a2r_pubsub_node<T>` / `start_r2a_pubsub_node<T>`) が
    static link 済みで存在する
  - **本タスクで追加**: agnocastlib に **「型名 → factory 関数ペア」の process-local
    table** を新設し、`Publisher<T>` / `Subscription<T>` の constructor で
    `(型名, &start_a2r_pubsub_node<T>, &start_r2a_pubsub_node<T>)` を自動登録
    (内部処理、ユーザコード非影響)
  - **動作フロー**: daemon は gossip data の `pid` で bridge 立て先 process を特定 →
    該当 process の bridge_manager に MQ msg (型名 + 方向 + topic) を送る。
    bridge_manager は同 process 内の table を lookup して factory を取り出し
    bridge を生成

これにより既存 `MqMsgBridge` の factory pointer フロー (要求元 process 依存) は
温存しつつ、daemon 起源の型名ベース要求も両モードで処理できる。

`MqMsgDaemonBridge` の具体的な field 並び、MQ 名、registry の C++ 表現、
bridge_manager の dispatch 詳細は実装 PR で扱う。

## 4. 互換性

| コンポーネント | F1 変更 | 境界 |
| --- | --- | --- |
| heaphook | 変更なし | ✅ patch |
| 既存 ioctl (command / struct / semantic) | 変更なし | ✅ patch |
| agnocastlib 公開 API (`Publisher<T>` / `Subscription<T>` のシグネチャ等) | 変更なし | ✅ patch |
| agnocastlib 内部 (`Publisher<T>` / `Subscription<T>` constructor) | process-local 型レジストリへの登録を追加 (1 行、ユーザコード非影響) | ✅ patch |
| agnocastlib 内部 (型レジストリ実装) | 新規 (process-local static table + template helper) | ✅ patch (内部のみ) |
| agnocastlib 内部 (bridge_manager) | 新 MQ を追加 listen + registry / plugin loader 経由で factory 解決、既存 MQ フローは不変 | ✅ patch (addition-only) |
| 既存 `MqMsgBridge` / `MqMsgPerformanceBridge` | 不変 | ✅ patch |
| 新 msg `MqMsgDaemonBridge` + 新 MQ | 新規 | ✅ patch (additive) |
| kmod | 変更なし | ✅ patch |
| 新パッケージ `ros2agnocast_discovery_msgs` | F3 と shared | ✅ patch |
| 新パッケージ `ros2agnocast_discovery_agent` (Python rclpy daemon) | F3 と shared、F1 で bridge 判定・MQ 送信を追加 | ✅ patch |

**バージョン組み合わせ互換性:**

- 旧 agnocastlib + 新 agnocastlib 混在: 旧側は新 MQ を listen しないので bridge 要求が
  通らない。NS-A だけ新版で NS-B が旧版なら NS-B 側に bridge が立たない → F1 機能
  しないが crash はしない (graceful degradation)。Atomic 更新前提なので過渡期共存は
  scope 外 (Imai-san 確認済み)

## 5. テスト戦略

- **単体**: bridge 要否判定の入力組合せ (pub-only / sub-only / 双方 / 両 NS) でユニットテスト
- **e2e (single host)**: `unshare --ipc` で 2 NS 作成、片方に agnocast pub、もう片方に agnocast sub を置いて pub→sub の通信成立を確認
- **e2e (multi-host)**: 2 VM + 同 `ROS_DOMAIN_ID` で cross-ECU 通信
- **on-demand 性**: 片方しか endpoint がない状態では bridge が立たないこと
- **cleanup**: 片方の endpoint 終了で bridge が片付くこと
- **mode 両対応**: Standard / Performance 双方で実行
- **Regression**: 既存 intra-NS 挙動に変化なし (`e2e_test_1to1.bash` 等)

## 6. 検討した代替案

- **per-IPC daemon を agnocastlib `poll_for_unlink` の C++ 拡張で実装** (IMAI 叩き台が想定): 利点は agnocastlib の既存 per-IPC NS daemon (起動・二重起動防止 lifecycle) と bridge_manager との同言語・同プロセス境界の MQ 通信を流用できる点。欠点は agnocastlib に rclcpp 依存を持ち込み C++ 改修範囲が広いこと。本 doc では別プロセス Python 案を採用、長期的には C++ 統合の余地あり
- **Standard bridge_manager に plugin loader を移植** (Performance と factory 解決を統一): 長期的に Standard / Performance の差分が縮まる利点はあるが、既存ユーザの CMake に plugin `.so` emit 設定が必要でユーザ側 build 更新の調整コストが大きい。本 doc は Standard を process-local registry で解決
- **daemon は trigger だけ送り、user process が `MqMsgBridge` を自己発行**: 既存 MQ プロトコル温存できるが、user process ごとに listener thread 常駐 + bridge 生成判断が daemon と user process に分散して race window が発生する
- **daemon が直接 bridge node を生成 (bridge_manager をバイパス)**: 責務が daemon に漏れ、Standard / Performance plugin 解決を daemon が再実装する負担。生成は bridge_manager に任せる
- **kmod に bridge ルール登録 ioctl 追加**: 新 ioctl = `need-minor-update` 化、`need-patch-update` 維持を優先
- **Python から MQ 送信を C 拡張で wrap** (`pybind11` 等): `posix_ipc` 直叩きの binary serde が保守性課題化したら検討

## 7. Appendix: IMAI 案との差分

IMAI 叩き台 ([https://tier4.atlassian.net/wiki/x/fIDcMwE](https://tier4.atlassian.net/wiki/x/fIDcMwE)) の F1 部分 / 共通基盤からの主な変更点:

| 項目 | IMAI 案 | F1 本 doc |
| --- | --- | --- |
| daemon 配置 / 実装 | per-IPC NS、agnocastlib 内 `poll_for_unlink` を C++ 拡張 | per-IPC NS は同じ。**実装は別パッケージ** `ros2agnocast_discovery_agent` (Python rclpy 別プロセス) で agnocastlib に rclcpp 依存を持ち込まない。C++ 統合案は §6 で trade-off を整理 |
| 通信ランドスケープ軸 | IPC NS と net NS を 1 軸に圧縮 (§3.1 注記) | **独立軸**として扱い、(same ECU, diff IPC NS, same net NS) を一級ケースに含める |
| Case 3-4 (same ECU, diff NS) | ☓ (ns 分離で遮断) | net NS 共有の場合 ✅、net NS も分離の場合のみ ☓ |
| bridge 指示の MQ | 既存 `MqMsgBridge` を投入 | 既存 `MqMsgBridge` は不変、**新 MQ +** `MqMsgDaemonBridge` を別建て (Standard モードで daemon が factory pointer を埋められない問題のため) |
| Gossip schema | 未確定 | `AgnocastDaemonState` / `AgnocastTopic` (`type_name` 含む) / `AgnocastEndpoint` を本 doc §3.3 で確定 |
| F2 関連 (domain_id, DomainBridgeRule ioctl) | 共通基盤として一緒に記述 | scope 外 (F2 タスクで minor-update として別途) |

## 8. 実装後追記: バージョン境界と将来計画

技術的には kmod を拡張する `need-minor-update` が最もクリーンだが、リリース
速度を優先して **本リリースは `need-patch-update` 範囲で頑張る** ことに決定
(Imai-san 2026-05-20 確認済み。[Slackスレッド](https://star4.slack.com/archives/C07FL8616EM/p1779253924516359))。

本章では (a) minor が技術的には理想だった理由、(b) 今回 patch で進めるにあたり
許容する drawback、(c) patch 経路の選択肢、(d) 次リリースでの kmod 移行計画
を記録する。

### 8.1 本来は minor (kmod 拡張) が理想だった理由

下記の 5 ステップが連鎖していて、`type 名` を daemon に届ける経路としては
kmod 経由が最もクリーン:

1. **bridge_manager が bridge node を生成するには `type 名` が必須**
   ROS 2 の Publisher / Subscription は型ごとに別実装になっており (`rclcpp::Publisher<MessageT>`)、
   factory 関数も型ごとに別 instance (`start_a2r_pubsub_node<T>` 等)。bridge_manager が
   どの factory を呼ぶか決めるには `std_msgs/msg/Int32` のような type 名が要る。

2. **`bridge を立てろ` のリクエストを発行するのは daemon (`ros2agnocast_discovery_agent`)**
   §3.2 の通り、自 NS と他 NS の状態を突合して bridge 要否を判定するのは daemon。
   bridge_manager は MQ で来た要求を受けて生成するだけ → リクエスト msg
   (`MqMsgDaemonBridge`) には type 名を **daemon が埋める必要** がある。

3. **daemon は user プロセス (Publisher<T> / Subscription<T> を持つプロセス) とは別プロセス**
   daemon は agnocastlib に link されていない独立した Python rclpy ノード。type 名は
   `Publisher<T>` の C++ template 引数として **user プロセス内のコンパイル時にしか
   存在しない**情報なので、daemon は何らかの IPC 経由で受け取らないと知り得ない。

4. **user プロセス → daemon の経路で利用可能なのは kmod 経由のみ**
   両プロセスとも同 IPC namespace に居て、kmod は既に NS-scoped に pub/sub 状態を
   保持・露出している (`topic_info_ret` で QoS / node_name 等を渡す既存経路)。新規 shm
   や socket を作るより、既存の経路に type 名を相乗りさせるのが圧倒的に小さい。

5. **しかし現状の kmod は type 名を保持・露出する仕組みを持たない**
   `topic_info_ret` には `node_name` / `qos_*` / `is_bridge` しか無く、`message_type`
   フィールドが無い。agnocastlib も pub/sub 登録時 ioctl で type 名を kmod に渡して
   いない。両方に追加が要る。

→ 最小サーフェスの修正は **既存** `topic_info_ret` struct に `char message_type[256]`
を追加 + agnocastlib が登録時 ioctl に type 名を載せる。既存 ioctl struct を
変更するため `need-minor-update` に該当する。加えて lifecycle (プロセス死亡時の
state cleanup) も kmod の `do_exit` hook で自動で済むため、sync の特別実装も不要。

### 8.2 今回の方針: patch 範囲で頑張る

本リリースは UDS 等の patch 範囲の機構で実装し (具体機構は §8.3)、以下の前提で
state sync 課題をスコープ外にする:

- **Autoware 起動前に daemon が立ち上がっている** (late-start なし)
- **daemon は実行中に死なない** (restart 時の state 再構築なし)

daemon が死んだことに気付けるよう、**ログ出力** と **問い合わせコマンド** を
併せて提供する。今回の patch 経路は **transitional な実装**。

### 8.3 選択肢と評価

| 機構 | 仕組み | 評価 |
| --- | --- | --- |
| **UDS** (Unix domain socket) | process → daemon に直接送信、`SO_PEERCRED` で pid 取得可 | ○ 採用候補 |
| **POSIX MQ** | per-NS の MQ に process が write、daemon が read | ○ 採用候補 (queueing あり、本 lib で既存利用あり) |
| **tmpfs file** | `/run/agnocast/<pid>.json` に書く、daemon が走査 | ○ 採用候補 (file persist で耐性高い) |
| **named POSIX shm** | shm 版の file 案 | ○ 採用候補 (file 版と等価) |
| 新 ioctl `GET_TOPIC_INFO_V2` を additive に追加 | kmod 不変は表面的に patch 寄り | ✗ `is_version_consistent` が kmod ↔ agnocastlib の major/minor 一致を要求するため結局 minor 連動 |
| Gossip だけで対処 (片側が知れば他側に伝わる) | 既存 `/_agnocast_discovery` を流用 | ✗ コールドスタートで誰も type を知らない (chicken-and-egg) |
| DDS の type hash / rmw_gid 一致で factory を選ぶ | string serialize 不要 | ✗ `Publisher<T>` の rmw_gid は type 非依存 (§3) ので逆引き不可 |

今回の前提下では ○ の 4 つはどれでも成立。具体機構は実装単純さと既存
agnocastlib のスタイル整合で実装 PR にて確定。

### 8.4 次リリースで kmod に移行 (確定方針)

次の minor リリースで以下に統一:

- `topic_info_ret` への `message_type` フィールド追加 (§8.1 の本筋)
- daemon は型情報を kmod から **topic 名 / node 名と同じ polling 経路** で取得
- 本リリースで導入する patch 経路は廃止、kmod `do_exit` の自動 cleanup で sync
  問題を根本解決
