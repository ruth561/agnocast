# PR1350 レビュー Session 1: schema / package 境界レビュー

## 目的

msg schema と各パッケージの build/export 定義を確認し、後続 PR (#1351–#1353) への
土台として破綻がないか判定する。

## 対象ファイル

- `src/ros2agnocast_discovery_msgs/msg/AgnocastEndpoint.msg`
- `src/ros2agnocast_discovery_msgs/msg/AgnocastTopic.msg`
- `src/ros2agnocast_discovery_msgs/msg/AgnocastDaemonState.msg`
- `src/ros2agnocast_discovery_msgs/CMakeLists.txt`
- `src/ros2agnocast_discovery_msgs/package.xml`
- `src/ros2agnocast_discovery_agent/package.xml`
- `src/ros2agnocast_discovery_agent/setup.py`

## 実装の現状

### AgnocastEndpoint.msg

```
string node_name
int32 pid
uint32 qos_depth
bool qos_is_transient_local
bool qos_is_reliable
bool is_bridge
```

- `pid` は agnocast_kmod の既存 ioctl が公開していないため best-effort 0

### AgnocastTopic.msg

```
string topic_name
string type_name   # best-effort 空; ioctl/kmod が未対応なので将来拡張
uint32 domain_id   # reserved; kmod が single domain なので現状常に 0
AgnocastEndpoint[] publishers
AgnocastEndpoint[] subscribers
```

### AgnocastDaemonState.msg

```
uint32 schema_version
string agnocast_version          # debug/互換確認用冗長フィールド
string host_uuid                 # /etc/machine-id ベース推奨
string host_hostname
builtin_interfaces/Time timestamp   # stale 判定に使用
uint64 ipc_ns_inode              # このメッセージが表す IPC NS
AgnocastTopic[] topics
```

### パッケージ定義

`ros2agnocast_discovery_msgs`:
- `ament_cmake` / `rosidl_default_generators` で msg 生成
- 依存は `builtin_interfaces` のみ

`ros2agnocast_discovery_agent`:
- `ament_python` / `exec_depend`: `rclpy`, `ros2agnocast_discovery_msgs`, `agnocast_ioctl_wrapper`
- `test_depend`: `ament_copyright`, `ament_flake8`, `ament_pep257`, `python3-pytest`

## 設計ドキュメントとの対照

design-doc §3.3 で定義されたスキーマ:

| field | 設計 | 実装 |
| --- | --- | --- |
| `schema_version` | ✅ | ✅ |
| `agnocast_version` | ✅ debug 保険 | ✅ |
| `host_uuid` | ✅ | ✅ |
| `host_hostname` | ✅ | ✅ |
| `timestamp` | ✅ stale 判定 | ✅ |
| `ipc_ns_inode` | ✅ | ✅ |
| `topics[]` | ✅ | ✅ |
| `type_name` (AgnocastTopic) | ✅ 型表示に使う | ⚠️ best-effort 空 (ioctl 未対応) |
| `pid` (AgnocastEndpoint) | ✅ bridge pid lookup 用 | ⚠️ best-effort 0 (ioctl 未対応) |
| `domain_id` | ✅ | ⚠️ reserved 0 (kmod が single domain) |

`type_name`, `pid`, `domain_id` が 0/空であることは PR 本文・コメントに明示済み。

## レビュー観点

### ✅ 確認すること

1. **schema_version 運用の一貫性**  
   コメントには "v1 initial form; future bumps add fields without breaking existing readers" とある。
   ただし v2 への bump ポリシーが design-doc では "互換破壊版で bump" と書かれており、
   non-breaking addition に関する挙動が schema コメントと完全には一致しない。
   将来の受信側が unknown field を安全に無視できる保証はどこで担保するか？

2. **`host_uuid` の一意性保証**  
   `/etc/machine-id` ベース推奨だが、コンテナ内で machine-id がホストと同一のケースや
   空ファイルのケースを fallback が正しく処理できるか。
   (実装側は Session 2 で確認するが、schema レベルでの意味の記述が十分か)

3. **`ipc_ns_inode` の collision リスク**  
   uint64 のままで IPC NS inode を識別子として使うことに設計的な制限はないか。
   同一 host で NS を作り直すと inode が再利用される可能性は？

4. **`pid` の型: int32 vs uint32**  
   Linux PID は 1–4194304 の正整数だが int32 になっている。
   符号付きを選んだ意図（sentinel -1 を使う予定がある？）の確認。

5. **`agnocast_version` の populate 経路**  
   agent は `os.environ.get('AGNOCAST_VERSION', '')` で取得。
   この環境変数が設定されない場合は空文字になるが、debug 用フィールドとして
   十分な使いやすさがあるか。

6. **`exec_depend` に `agnocast_ioctl_wrapper` を宣言している**  
   `agnocast_ioctl_wrapper` は shared library パッケージ。
   Python ament_python パッケージが exec_depend としてこれを持つことで、
   ament がインストール時に `.so` を正しく引けるか確認。

7. **`test_depend` の lint 系が全部入っているか**  
   `ament_mypy` が test_depend に含まれていない。
   型チェックが意図的に除外されているか。

### ⬛ 今回の範囲外

- `type_name` / `pid` / `domain_id` が空/0 であることの是非 → 既知の設計差分
- schema v2 以降の具体的な拡張内容

## 期待アウトプット

- schema 由来の将来互換性リスク: 有/無、重大度
- package 定義の漏れ: 有/無
- 後続 PR (#1351–#1353) がこの schema を使う際の前提条件の不明点
