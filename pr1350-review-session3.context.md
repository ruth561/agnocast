# PR1350 レビュー Session 3: gossip / lifecycle レビュー

## 目的

`/_agnocast_discovery` の publish・subscribe・stale state 管理・shutdown の挙動が、
「shared base」として後続 PR が乗れる最低限の品質を満たしているか確認する。

## 対象ファイル

- `src/ros2agnocast_discovery_agent/ros2agnocast_discovery_agent/agent.py`
  - `_gossip_qos()`
  - `DiscoveryAgent.__init__()`
  - `DiscoveryAgent._on_tick()`
  - `DiscoveryAgent._prune_stale_remote_states()`
  - `DiscoveryAgent.publish_snapshot()`
  - `DiscoveryAgent.build_state()`
  - `DiscoveryAgent._on_remote_state()`
  - `main()`

## 実装の現状

### QoS

```python
def _gossip_qos() -> QoSProfile:
    return QoSProfile(
        reliability=ReliabilityPolicy.RELIABLE,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
        history=HistoryPolicy.KEEP_LAST,
        depth=1,
        liveliness=LivelinessPolicy.AUTOMATIC,
        liveliness_lease_duration=Duration(seconds=LIVELINESS_LEASE_SEC),  # 30s
    )
```

### timer / tick

```python
self._timer = self.create_timer(PUBLISH_INTERVAL_SEC, self._on_tick)  # 1.0s

def _on_tick(self) -> None:
    self._prune_stale_remote_states()
    self.publish_snapshot()
```

### self-message skip

```python
def _on_remote_state(self, msg: AgnocastDaemonState) -> None:
    if msg.host_uuid == self._host_uuid and msg.ipc_ns_inode == self._ipc_ns_inode:
        return
    self._remote_states[(msg.host_uuid, msg.ipc_ns_inode)] = msg
```

### stale prune

```python
def _prune_stale_remote_states(
        self, now_sec: float | None = None,
        stale_after_sec: float = REMOTE_STATE_STALE_SEC) -> None:  # 30.0s
    if now_sec is None:
        now_sec = self.get_clock().now().nanoseconds / 1e9
    stale_keys = [
        key for key, msg in self._remote_states.items()
        if now_sec - (msg.timestamp.sec + msg.timestamp.nanosec / 1e9)
        > stale_after_sec
    ]
    for key in stale_keys:
        del self._remote_states[key]
```

### shutdown

```python
def main(argv=None) -> int:
    rclpy.init(args=argv)
    node = DiscoveryAgent()
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
    return 0
```

## 設計ドキュメントとの対照

| 設計 | 実装 | 状態 |
| --- | --- | --- |
| Reliable + TransientLocal + KeepLast(1) + Liveliness Automatic 30s | `_gossip_qos()` | ✅ |
| 1 daemon = 1 IPC NS | `_ipc_ns_inode` で自己識別 | ✅ |
| 変化検知時即 publish + heartbeat 0.1 Hz | 1 Hz timer-only | ⚠️ 既知差分 (follow-up) |
| remote state stale 管理 | `_prune_stale_remote_states` | ✅ |
| self message skip | `_on_remote_state` guard | ✅ |

## レビュー観点

### ✅ 確認すること

1. **publish と subscribe が同じ QoS を使っている**  
   publisher と subscriber 両方に `_gossip_qos()` を適用しており、
   TransientLocal 同士でのみ late-joiner が受信できる。
   他 NS の subscriber が異なる QoS を使う場合に silent failure になる余地はないか。
   (e2e test は QoS 一致前提で書かれているが、manual test 手順では QoS オプション
   付きの `ros2 topic echo` が必要なことが caveat として明記されているか確認)

2. **self-message skip の一意性判定**  
   `(host_uuid, ipc_ns_inode)` で自己識別。TransientLocal なので自分の publish を
   自分が受信するタイミングがある。このペアが衝突しない前提が成立する条件は何か。
   特に同一 host で IPC NS を作り直した場合の inode 再利用との組み合わせ。

3. **stale 判定が sender の clock に依存している**  
   `msg.timestamp` は sender 側の `get_clock().now()` で埋めており、
   receiver 側の clock と乖離がある場合に「まだ有効なのに stale 扱い」または
   「既に死んでいるのに有効扱い」が起きる。
   システム clock が調整される環境での挙動は許容されているか。

4. **`_prune_stale_remote_states` が 1 Hz でのみ走る**  
   DDS Liveliness が失効して publisher が消えても、最大 30s 後のティックまで
   `_remote_states` から削除されない。この遅延は bridge 生成判定の latency に
   直結するが、PR1350 時点では bridge decider 未実装なので問題にならない。
   後続 PR (#1353) への引き継ぎ事項として明記があるか。

5. **`destroy_node` の順序**  
   `finally` で `destroy_node()` を呼んだ後に `rclpy.shutdown()` を呼ぶ順序は正しい。
   ただし `destroy_node()` が例外を投げる場合、`rclpy.shutdown()` は呼ばれない。
   `rclpy.ok()` チェックを挟んでいるので致命的ではないが、二重 shutdown になる
   ケースはないか。

6. **`DiscoveryAgent.__init__` の mid-failure**  
   `_load_ioctl_wrapper()` や `_read_ipc_ns_inode()` が例外を投げた場合、
   `create_publisher` が呼ばれる前に死ぬので publisher リソースリークはない。
   ただし `rclpy.init()` は既に呼ばれているので `rclpy.shutdown()` は
   `main()` の finally で呼ばれる必要がある。この経路で `node` が未定義に
   なって `finally` ブロックの `node.destroy_node()` が NameError になるか確認。

7. **`remote_states` property の公開**  
   外部からの読み取り専用 dict として `@property` で公開されているが、
   caller が直接 dict を mutate できる。後続 PR で bridge decider がこれを
   使う際の安全性について。

8. **`build_state()` と `publish_snapshot()` の責務分離**  
   `build_state()` が public になっているのは後続 PR やテストからの呼び出しを
   意図しているか。テスト側からは実際に `build_state()` を使っているか確認。

### ⬛ 今回の範囲外

- change-detection による即 publish の実装 → 既知差分、follow-up PR
- bridge 要否判定ロジック → 後続 PR (#1353)

## 期待アウトプット

- shared base として後続 PR に引き渡す前に直すべき lifecycle 上の欠陥の有無
- stale 管理・shutdown の現状実装の妥当性評価
- 後続 PR への引き継ぎ事項の明示が不足している箇所
