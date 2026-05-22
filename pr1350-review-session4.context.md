# PR1350 レビュー Session 4: launch / integration レビュー

## 目的

daemon の launch 組み込み設計が「1 IPC namespace に 1 daemon」という運用前提を
崩さないか、また sample application への include が後続 e2e の前提として妥当かを確認する。

## 対象ファイル

- `src/ros2agnocast_discovery_agent/launch/discovery_agent.launch.xml`
- `src/agnocast_sample_application/launch/talker.launch.xml`
- `src/agnocast_sample_application/launch/listener.launch.xml`

## 実装の現状

### discovery_agent.launch.xml

```xml
<launch>
  <!--
    Spawn the per-IPC-namespace Agnocast discovery agent. Include this from
    any top-level launch that runs inside an IPC namespace where Agnocast
    observability and bridge generation are desired. Exactly one daemon
    should run per IPC namespace.
  -->
  <node pkg="ros2agnocast_discovery_agent"
        exec="discovery_agent"
        name="agnocast_discovery_agent"
        output="screen"/>
</launch>
```

### talker.launch.xml (変更部分)

```xml
<arg name="discovery_agent" default="true"/>
<include file="$(find-pkg-share ros2agnocast_discovery_agent)/launch/discovery_agent.launch.xml"
         if="$(var discovery_agent)"/>

<node pkg="agnocast_sample_application" exec="talker" name="talker_node" output="screen">
    <env name="LD_PRELOAD" value="libagnocast_heaphook.so:$(env LD_PRELOAD '')" />
</node>
```

### listener.launch.xml (変更部分)

```xml
<arg name="discovery_agent" default="true"/>
<include file="$(find-pkg-share ros2agnocast_discovery_agent)/launch/discovery_agent.launch.xml"
         if="$(var discovery_agent)"/>

<node_container pkg="agnocast_components" exec="agnocast_component_container" ...>
    ...
</node_container>
```

## 運用前提の整理

- 1 IPC namespace につき daemon を 1 プロセスだけ起動する (設計 §3.2)
- 起動責任はユーザ (systemd unit / `ros2 launch` / container entrypoint 等)
- daemon 二重起動の防止は launch ファイルの `discovery_agent:=false` フラグに依存

## レビュー観点

### ✅ 確認すること

1. **talker + listener を同一 IPC namespace で両方起動すると daemon が 2 つ立つ**  
   talker.launch.xml も listener.launch.xml も `discovery_agent` のデフォルトが
   `true` になっている。
   同一 IPC NS で talker と listener を別々の launch で起動すると daemon が 2 つ立つ。
   PR 本文や launch コメントには「1 NS に 1 daemon」と書かれているが、
   それを `false` にする責任がユーザに完全に委ねられている。
   二重起動への guard や警告が launch レベルで存在しないことは許容か。

2. **`discovery_agent.launch.xml` を include する構造で名前衝突が起きるか**  
   node 名が `agnocast_discovery_agent` に固定されている。
   ROS 2 の node 名前空間が別であれば 2 つ起動できてしまう。
   同一名前空間で起動した場合は ROS 2 が警告を出すが、launch が止まるわけではない。

3. **sample application への include の意図が明確か**  
   `agnocast_sample_application` は本来 discovery agent と無関係なはず。
   sample application に daemon を組み込んだことが「e2e 手順をシンプルにする」
   目的と理解できるが、PR 本文の manual test 手順では
   talker.launch が daemon を含むことが明記されているか確認。

4. **`$(env LD_PRELOAD '')` の空 fallback が discovery_agent.launch に伝播しないか**  
   include された discovery_agent.launch.xml の node は `LD_PRELOAD` を指定していない。
   heaphook が discovery_agent に preload されないことは正しいか
   (agnocastlib を使わない Python rclpy node なので不要のはず)。

5. **`discovery_agent.launch.xml` に起動引数 (node_namespace 等) がない**  
   node を namespace 下に置けないため、複数の IPC NS で同じ ROS 2 domain を
   共有したときに node 名が衝突する。
   これは今後の拡張で対処される想定か、それともこの PR での制約として記録すべきか。

6. **listener が node_container + composable_node の構成になっている**  
   `discovery_agent` は通常の node として起動される。
   listener が component container の中に居る場合、2 プロセスが同 IPC namespace に
   存在し、discovery_agent は listener_container プロセスの Agnocast endpoint を
   正しく観測できるか (ioctl は caller の IPC NS 全体をスキャンするため問題ないはず)。

### ⬛ 今回の範囲外

- Autoware 環境での launch 統合
- 二重起動防止のための kmod/agnocastlib 側の機構 → 後続 PR / 将来機能

## 期待アウトプット

- 二重起動リスクの severity 評価と、現状 launch API での軽減手段
- sample application への組み込みが後続 e2e の前提として整合しているかの評価
- 後続 launch 統合 (Autoware / systemd) に向けた open issue の整理
