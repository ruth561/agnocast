# PR1350 Review Plan

## 1. 目的

PR1350 は、cross-IPC-namespace / cross-ECU 対応の本体機能ではなく、後続 PR の土台を追加する PR である。

- `ros2agnocast_discovery_msgs`: IPC namespace 1つ分の Agnocast state を表現する msg 群
- `ros2agnocast_discovery_agent`: 既存 ioctl wrapper を読んで `/_agnocast_discovery` に state を publish する per-IPC daemon

この Markdown の目的は、PR1350 のレビューを「何を確認し、何を今回の範囲外とみなすか」という観点で整理し、個別レビュー Session のベースにすること。

## 2. この PR の責務境界

### 今回レビュー対象

1. discovery msg schema が設計意図に沿っており、後続 PR (#1351, #1352, #1353) の入力として破綻しないこと
2. discovery agent が既存の NS-scoped ioctl wrapper を安全に読み、IPC namespace 単位の snapshot を正しく組み立てて publish すること
3. gossip publish の QoS / identity / timestamp / stale-state 管理が、この PR の責務として妥当であること
4. launch 組み込みと sample application への include が、「1 IPC namespace に 1 daemon」という運用前提を壊さないこと
5. テストが、この PR が主張する保証範囲を最低限カバーしていること

### 今回の範囲外

1. bridge auto-generation 自体の正しさ
2. CLI 側が gossip を subscribe / merge / 表示する処理
3. agnocastlib / heaphook / kmod / MQ payload の変更有無に関する深掘り実装レビュー
4. 設計 doc 上の将来案のうち、まだこの PR で実装していないもの

### 既知の設計差分として受容済みの点

以下は PR 本文で明示されているため、原則として「未実装」だけでは指摘対象にしない。

- publish 戦略は「change-detection + heartbeat」ではなく、現時点では 1 Hz timer-only
- `type_name` は best-effort で空文字になり得る
- `pid` は既存 ioctl に存在しないため endpoint msg では 0 を入れる
- cross-ECU の実機確認は未完了

## 3. 変更ファイルの整理

### 中心実装

- `src/ros2agnocast_discovery_agent/ros2agnocast_discovery_agent/agent.py`

### schema / package 定義

- `src/ros2agnocast_discovery_msgs/msg/AgnocastEndpoint.msg`
- `src/ros2agnocast_discovery_msgs/msg/AgnocastTopic.msg`
- `src/ros2agnocast_discovery_msgs/msg/AgnocastDaemonState.msg`
- `src/ros2agnocast_discovery_msgs/CMakeLists.txt`
- `src/ros2agnocast_discovery_msgs/package.xml`
- `src/ros2agnocast_discovery_agent/package.xml`
- `src/ros2agnocast_discovery_agent/setup.py`

### launch / integration

- `src/ros2agnocast_discovery_agent/launch/discovery_agent.launch.xml`
- `src/agnocast_sample_application/launch/talker.launch.xml`
- `src/agnocast_sample_application/launch/listener.launch.xml`

### test / smoke

- `src/ros2agnocast_discovery_agent/test/test_agent.py`
- `scripts/test/e2e_test_discovery_agent.bash`

## 4. レビューの主仮説

レビューでは、以下の仮説を順に潰す。

### 仮説A: schema が後続 PR の前提を満たしている

確認したい点:

- `AgnocastDaemonState` が「1 daemon = 1 IPC namespace = 1 snapshot」という設計に沿っているか
- stale 判定や識別に最低限必要な field (`schema_version`, `host_uuid`, `timestamp`, `ipc_ns_inode`) が揃っているか
- `AgnocastTopic` / `AgnocastEndpoint` が ioctl wrapper の shape と整合しているか
- v1 schema とコメントが、将来拡張ポリシーと矛盾していないか

レビューで出しやすい論点:

- field の不足や命名の曖昧さが後続 PR の複雑化を招かないか
- `type_name=''`, `domain_id=0`, `pid=0` という best-effort 値の意味が schema 利用側に十分伝わるか

### 仮説B: agent の snapshot 構築は安全で責務が過不足ない

確認したい点:

- `ctypes` の struct 定義が wrapper 側の ABI と噛み合っているか
- topic list と endpoint list の取得後に free が必ず走るか
- ioctl が空を返す場合に空 snapshot として正常化されるか
- 文字列 decode の失敗や欠損が agent の停止要因にならないか

レビューで出しやすい論点:

- `TopicInfoRet` が wrapper header と本当に一致しているか
- `_read_machine_id()` の fallback が mixed host 環境や再起動時の期待と矛盾しないか
- 現時点で未解決の型情報不足が、この PR の責務範囲で適切に閉じているか

### 仮説C: gossip publish / remote-state cache の挙動が薄い土台として妥当

確認したい点:

- QoS が設計どおり `Reliable + TransientLocal + KeepLast(1) + Automatic liveliness 30s` か
- self message を cache に入れず、remote state だけ保持する実装になっているか
- stale remote state の prune が無限保持を避けるローカル防御として成立しているか
- `build_state()` と publish の責務分離が今後の bridge decider 追加に耐えるか

レビューで出しやすい論点:

- `remote_states` の管理はこの PR で本当に必要十分か
- stale 判定が timestamp 依存であることの妥当性
- 1 Hz timer-only でもこの PR の「shared base」としては十分か

### 仮説D: launch 組み込みは運用上の事故を増やさない

確認したい点:

- standalone launch と sample launch include の責務分離が明確か
- `discovery_agent:=false` の escape hatch が十分か
- 「1 IPC namespace に 1 daemon」をコメントと launch API で誤用しにくくできているか

レビューで出しやすい論点:

- talker/listener の両方で default true にした結果、既存 launch 合成時に二重起動を誘発しないか
- sample application に組み込んだことが、後続 e2e の前提として妥当か

### 仮説E: テストは主張している保証範囲と一致している

確認したい点:

- unit test が「ioctl -> msg 変換」「topic snapshot 組み立て」「machine-id fallback」を押さえているか
- e2e smoke が message shape と QoS の両方を確認しているか
- PR 本文の manual test 手順と自動テストの責務分離が明確か

レビューで出しやすい論点:

- stale-state prune や self-message skip など、今の実装差分に対するテストが不足していないか
- e2e が TransientLocal subscriber 側の QoS 指定抜けで false negative にならないように書かれているか

## 5. Session の切り方

レビューは以下の 5 Session に分割すると進めやすい。

### Session 1: schema / package 境界レビュー

対象:

- discovery msgs 3件
- `ros2agnocast_discovery_msgs` の build 定義
- `ros2agnocast_discovery_agent` の package 定義

主に見ること:

- schema の field 妥当性
- コメントと実際の利用目的の整合
- build/export 依存の不足や過剰

期待アウトプット:

- msg schema 由来の将来互換性リスクの有無
- package 定義の漏れの有無

### Session 2: agent の snapshot 変換レビュー

対象:

- `agent.py` の `TopicInfoRet`, `_load_ioctl_wrapper()`, `_ioctl_to_endpoint()`, `_collect_endpoints()`, `read_local_topics()`

主に見ること:

- ABI 仮定の危うさ
- null / empty / decode error 時のふるまい
- free 漏れ、例外時 cleanup 漏れ

期待アウトプット:

- ioctl wrapper 依存の実装として安全か
- follow-up PR 前に直すべき変換層の不備があるか

### Session 3: gossip / lifecycle レビュー

対象:

- `agent.py` の QoS, timer, `build_state()`, `_on_remote_state()`, `_prune_stale_remote_states()`

主に見ること:

- publish 周期と設計差分の扱い
- remote state cache の必要性と妥当性
- shutdown / restart / stale data まわりの最小防御

期待アウトプット:

- shared base として十分か
- 後続 PR が乗る前に潰すべき lifecycle 上の欠陥があるか

### Session 4: launch / integration レビュー

対象:

- `discovery_agent.launch.xml`
- sample application の talker/listener launch

主に見ること:

- daemon 二重起動の誘発余地
- sample launch に組み込む判断の妥当性
- 運用コメントが誤用防止に足りているか

期待アウトプット:

- launch API と運用前提の齟齬の有無

### Session 5: test / release claim レビュー

対象:

- `test/test_agent.py`
- `e2e_test_discovery_agent.bash`
- PR 本文の tested / caveat / patch-update claim

主に見ること:

- テストが PR の主張を裏切っていないか
- `need-patch-update` の主張と変更実体が一致しているか
- reviewer note の manual step に見落としがないか

期待アウトプット:

- 不足テストの候補
- マージ前に要求すべき再現手順や補足説明の有無

## 6. 先に確認しておくべき観点

各 Session で共通して意識する。

1. この PR は「shared base」であり、後続 PR の実装都合がこの PR に漏れ込みすぎていないか
2. 逆に、shared base と言いながら後続 PR が必要とする最低限の情報が欠けていないか
3. additive-only という主張に対して、既存 runtime への副作用が launch 組み込み経由で入っていないか
4. コメントで説明している制約が、実装またはテストにきちんと反映されているか

## 7. レビュー開始時の実用コマンド

```bash
gh pr checkout 1350
gh pr view 1350
gh pr diff 1350 --name-only
colcon test --packages-select ros2agnocast_discovery_agent
bash scripts/test/e2e_test_discovery_agent.bash
```

必要に応じて、manual test は PR 本文の `unshare --ipc` 手順を使う。

## 8. この Markdown を使った次アクション

1. 上の Session から 1 つ選び、その観点だけでレビューする
2. Findings は「仕様逸脱」「実装欠陥」「テスト不足」「説明不足」に分けて記録する
3. Session ごとの結論を最後に統合し、PR1350 全体のレビュー結果にまとめる