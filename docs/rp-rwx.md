# RP 与 RWX 深度差异分析

> 分析对象：`RWX/`（Java/Kotlin 参考实现）、`RP/`（C++17 + raylib 重制）、`pseudocode/`（仅作导航）。  
> 方法：源码逐段对照 + RP 自带无头门实测 + 资产/对象统计。  
> 约束：只读分析，未修改任何源码。  
> 日期：2026-11-21。
>
> **2026-11-21 晚补记**：本文是分析当天的快照。P0 里的「`dont_load`、`OVER_CLIFF`/`HOVER`、地图对象层、
> 队伍/经济/胜负、动作系统」以及 P1 的「迷雾公式、伤害顺序」已经修完并加了机器门，详见
> [§17 修复记录](#17-修复记录2026-11-21-晚)。下文保留原文以记录问题本身。

## 摘要

RP 是一个质量很高的“战斗内核移植”：炮塔、弹道、附件、运输、腿、修理/回收、地形与迷雾渲染都做得非常细，很多地方直接带着 RWX 的行号注释。它的问题主要不在战斗数学，而在**游戏规则层**和**平台层**。

这次深挖最关键的五个结论：

1. **地图对象层完全没读。** 84/85 张图有 `Triggers` 对象层，共 439 个对象；RP 源码里 `objectgroup`、`map_info`、`team_info`、`allyGroup`、`winCondition` 全部 0 命中。于是队伍结盟、地图 credits、胜负条件、任务类型、迷雾模式、相机起点、脚本触发器全部丢失。
2. **`dont_load` 没处理。** RWX 在解析前直接跳过这些模板；RP 却加载了 10 个 `dont_load` 文件，注册出 `null`、`example_faction_picker`、`custom_tank` 等不该存在的模板，最终让 `--checkbuiltins` 报出 1 条 BADACT。
3. **动作系统只实现了一个子集。** `fireTurretXAtGround`、`deleteSelf`、`resourceAmount`、`teleportTo` 等键被显式标成 unsupported；核弹发射动作因此是空操作。非 credits 的动作价格还会被当成 0。
4. **`OVER_CLIFF` 与 `HOVER` 的通行规则是反的。** 悬浮单位能穿熔岩/建筑，越崖单位反而不能越崖；A* 和实际移动都受影响。
5. **迷雾与 AI 视野不对称。** RP 的有效视野比 RWX 大约多 1.2–1.5 tile，外圈全部算可见；AI 则完全不受迷雾限制。玩家和 AI 的信息条件都不对。

一句话：**RP 是“能打的战斗沙盒”，还不是“完整的 RWX 复刻”。**

---

## 1. 方法与证据

### 1.1 对比方式

- 以 `RWX/core/src/main/java` 与 `RWX/core/src/main/kotlin` 为准，逐段核对 RP 的对应实现。
- `pseudocode/` 只用来定位架构，不作为行为依据。
- 用 RP 自带门做客观校验：
  - `--unusedkeys`：统计 INI 中未被读取的键；
  - `--checkmaps`：检查地图 tile/出生点/资源；
  - `--checkbuiltins`：检查内置模板与 canBuild 引用。
- 用 grep/find 统计资产、对象层、移动类型、动作键的实际使用量。

### 1.2 证据强度标记

本文中的结论分三类：

- **实测**：跑门或统计得到的具体数字；
- **源码**：可直接定位到文件/行号的实现差异；
- **推算**：由公式或计数推导出的影响，例如迷雾有效半径。

### 1.3 一个重要的前提

`--checkmaps: 0 of 85 need attention` 只说明 tile 层、出生点和资源没有明显问题，**不说明地图语义正确**。RP 的地图检查器没有解析 `<objectgroup>`，所以 84 张图的触发器/队伍/胜负信息即使全丢，这个门也会全绿。

---

## 2. 差异总览

| 级别 | 领域 | RWX | RP | 主要影响 |
|---|---|---|---|---|
| P0 | 地图对象层 | 解析 `Triggers` 中的 `map_info`、`team_info`、`attack_point` 等 | 完全不解析 | 25+ 张图胜负错误、29 个队伍结盟/经济错误、任务图变普通遭遇战 |
| P0 | 模板加载 | `dont_load` 在解析前返回 null | 无检查 | `null`/测试模板污染、菜单出现 `null`、BADACT |
| P0 | 动作系统 | 硬编码 + INI 动作完整 | 只支持转换/加资源/冷却等子集 | 核弹不能发射、8 个单位失去对地攻击、部分技能无效 |
| P0 | 队伍/经济/胜负 | allyGroup、地图 credits、mainBuilings | 全部队伍互敌、credits 硬编码、全灭判胜 | 1v3 变四家混战；地图平衡完全改变 |
| P0 | 寻路通行 | HOVER 越水/崖/大物体但怕熔岩；OVER_CLIFF 越崖但怕水 | HOVER 只查边界；OVER_CLIFF 当普通地面 | 悬浮穿熔岩/建筑，越崖单位不能越崖 |
| P1 | 迷雾/视野 | 内圈 i-3，外圈 0..10，>=5 隐藏；分队伍 | 内圈 i-2，外圈 1..4，<5 可见；单层 | 玩家视野更大；AI 全图 |
| P1 | 生产 | 按进度分批扣款、取消退已付、出厂锁定 | 下单全款、取消全退、无出厂锁 | 经济节奏与建造顺序不同 |
| P1 | AI | `[ai]`、aiUse、建筑、技能、运输 | 只会造兵、造矿、打一波 | AI 是占位水平 |
| P2 | 表现/平台 | 音频、UI、shader、任务、存档、联机、JVM mod | 缺失或 stub | 不是完整产品 |

---

## 3. 地图对象层：影响最大的整块缺失

### 3.1 数据规模

RP 的 TMX 加载器只读 tile layer 和 Units 层的 tile property；在 `src/` 中搜索 `objectgroup`、`map_info`、`team_info`、`allyGroup`、`winCondition`，没有任何命中。

对 85 张地图的统计：

| 对象/属性 | 数量 | 说明 |
|---|---:|---|
| 带 `Triggers` 对象层的地图 | 84 / 85 | 几乎全部地图 |
| `Triggers` 中的 object | 439 | 不只是元数据，还有脚本对象 |
| `map_info` | 81 | 地图类型、胜负条件、迷雾模式、介绍文本 |
| `team_*_info` | 59 | 队伍编号、credits、allyGroup |
| `allyGroup` 属性 | 29 | 结盟关系 |
| `winCondition = mainBuilings` | 17 张 | 只拆主基地 |
| `winCondition = none` | 8 张 | 沙盒/不判胜 |
| `winCondition = requiredObjectives` | 1 张 | 任务目标 |
| `type = mission / challenge / survival` | 13 / 8 / 5 张 | 非普通遭遇战 |
| `fog = map / los / none` | 22 / 1 / 3 张 | 地图定义迷雾模式 |
| `camera_start` | 8 个 | 开局相机位置 |
| `attack_point` | 26 个 | 进攻/巡逻点 |
| `move_team_*` | 33 个 | 团队移动触发器 |
| `rotate_units` | 10 个 | 单位旋转触发器 |

### 3.2 1v3 Island：结盟图变四家混战

以 `assets/maps/challenge/1v3 Island.tmx` 为例：

- `team_0_info`：`allyGroup=0`，credits 4000；
- `team_1_info`：`allyGroup=1`，credits 3000；
- `team_2_info`：`allyGroup=1`，credits 3000；
- `team_4_info`：`allyGroup=1`，credits 3500。

RWX 的语义是：team 1/2/4 三家结盟，一起打玩家。

RP 的语义是：

- `isEnemy(a,b)` 等价于 `a != b`，三家 AI 互为敌人；
- 玩家固定 4000，其他队伍固定 2500；
- 结果是一张“三家联合挑战玩家”的图，变成**四家自由混战**。

这不是小数值偏差，而是地图设计的核心机制被完全改变。

### 3.3 胜负条件

RP 的 `checkWinLoss`（`Game.cpp:3970-3976`）只做：

- team 0 没有单位 → DEFEAT；
- 所有非玩家队伍都没有单位 → VICTORY。

它不看 `mainBuilings`、`none`、`requiredObjectives`。

具体影响：

- **17 张 `mainBuilings` 地图**：RWX 只要求拆掉主基地，RP 要求清光所有单位和建筑；
- **8 张 `none` 地图**：本应不判胜，RP 仍会在敌人死光后弹 VICTORY；
- **1 张 `requiredObjectives` 地图**：RP 没有任务目标系统，无法按目标判定。

### 3.4 地图类型与任务

`map_info` 的 `type` 统计：

- 55 张 skirmish；
- 13 张 mission；
- 8 张 challenge；
- 5 张 survival。

RP 没有任务/触发器系统，这 26 张非普通遭遇战全部按 skirmish 加载：没有任务简报、没有目标、没有 `move_team`/`attack_point`/`rotate_units` 脚本。`introText` 也被忽略。

### 3.5 迷雾模式与相机

- 22 张 `fog=map`、1 张 `fog=los`、3 张 `fog=none`；
- RP 默认 `fogEnabled=true`，只有命令行 `--nofog` 能关；地图里的 `none` 不会生效；
- 8 张图的 `camera_start` 被忽略，开局相机落在 RP 自己的默认位置。

### 3.6 UnitObjects 的边界

`UnitObjects` 对象层只出现在两张图：

- `assets/maps/menu_background/menu3.tmx`；
- `assets/maps/testing/unit_types_test.tmx`。

普通地图的单位仍然放在 Units tile layer，RP 能读到；所以“普通地图丢单位”不是主要问题。**真正广泛丢失的是 `map_info` 和 `team_info` 这类规则数据。**

---

## 4. `dont_load`：模板被污染

### 4.1 RWX 的行为

RWX 在 `CustomUnitConfigParser.java:1184` 解析 INI 之前就检查：

```java
if (iniFile.getBoolean("core", "dont_load", false)) return null;
```

也就是说，这些文件只作为 `copyFrom`/模板片段存在，不应该注册成可玩单位。

### 4.2 RP 的行为

RP 没有这个检查，直接加载了 10 个带 `dont_load` 的文件。其中有名字的 5 个：

| 文件 | name | 后果 |
|---|---|---|
| `mech_large_common.ini` | `NULL` | 注册成 `null` |
| `mech_med_common.ini` | `NULL` | 与上一行互相覆盖 |
| `mechs_small_common.ini` | `NULL` | 与上一行互相覆盖 |
| `example_faction_picker.ini` | `example_faction_picker` | 注册测试单位 |
| `test_tank/tank.ini` | `custom_tank` | 注册测试坦克 |

另外 5 个没有 `[core] name`，RP 会因为 `name.empty()` 跳过，不会生成模板；但仍然会解析它们的内容。

### 4.3 具体后果

1. **`null` 变成真实模板。**
   `mechFactory` 的 `[canBuild]` 列表里有 `NULL:5`。RP 的 `parseBuildLinks` 只跳过 `"none"`（`Templates.cpp:553`），不跳过 `"NULL"`；而 `null` 模板恰好存在，于是机甲工厂的建造菜单里会出现一个 100 块的 `null` 单位。

2. **`--checkbuiltins` 报 BADACT。**
   `example_faction_picker` 带有一个不存在的动作引用 `bugNestN`：

   ```text
   BADACT example_faction_picker -> bugNestN
   checkbuiltins: 159 row(s), 1 bad reference(s)
   ```

   这条 BADACT 本身就是“加载了本不该加载的测试模板”的症状。

3. **测试单位进入模板列表。**
   `custom_tank` 会出现在模板/调试生成里，增加 159 行这个被污染的数量。

严格说，RP 的 `null` 只有 1 个（三个文件覆盖后），加上 `example_faction_picker`、`custom_tank`，至少 3 个模板不该存在。

---

## 5. 动作系统：核弹发射是空操作

### 5.1 被显式标记为 unsupported 的键

RP 的 `Templates.cpp:646-650` 有：

```cpp
for (const char* key : {"fireturretxatground", "teleportto",
                        "sendmessagewithdata",
                        "alsotriggerorqueueactionwithtarget",
                        "spawnunits", "attachments_addnewunits",
                        "deleteself", "resourceamount",
                        "setresourceswithlogic"})
    if (s.has(key)) a.supported = false;
```

`Game::runAction`（`Game.cpp:4222-4281`）实际只处理：

- `convertTo`
- `addResources`
- `addEnergy`
- 父护盾
- `setBuilt`
- 播放动画
- 动作冷却 / 全局冷却
- `alsoTriggerAction` / `alsoQueueAction`
- 发送消息

它**没有**：对地开火、传送、生成单位、删除自己、改资源。

### 5.2 出货单位受影响情况

| 未实现键 | 出货使用 | 后果 |
|---|---|---|
| `fireTurretXAtGround` | 8 个 INI | 失去手动对地攻击；核弹不能发射 |
| `deleteSelf` | `flare_10s` | 信号弹不会自动消失 |
| `resourceAmount` | 2 个 AA beam gunship | 资源量动作无效 |
| `sendMessageWithData` | `modular_spider/blink` | 带数据消息无效 |
| `alsoTriggerOrQueueActionWithTarget` | `modular_spider` | 带目标后续动作无效 |
| `takeResources` | `shieldGen` | 只有“父护盾”一个特例被支持 |
| `addResources : ammo=...` | 6 个单位 | 被标 unsupported；AI 不会用 |

使用 `fireTurretXAtGround` 的 8 个 INI：

- `nukes/nuke_launcher.ini`
- `modular_spider/artillery.ini`
- `modular_spider/blink.ini`
- `modular_spider/modular_spider.ini`
- `nautilus/nautilus.ini`
- `classic_bugs/bug_bee/bug_bee.ini`
- `experimental_gunship/experimental_gunship.ini`
- `missile_airship/missile_airship.ini`

### 5.3 核弹：从“能造”到“不能发”

`nuke_launcher.ini` 的 `[action_launchNuke]`：

```ini
fireTurretXAtGround : siloTop
fireTurretXAtGround_showGuideDecals : targetGuide
price : ammo=1
displayType : action
```

RWX 里这是硬编码的 `NukeLauncher.a(float,float)`（`NukeLauncher.java:338-382`）：

- 检查 `nukeCount <= 0`；
- 距离小于 202500（450px）时提示 “Nuke target too close”；
- 扣 `nukeCount`；
- 生成 `nukeProjectile`；
- 播放发射音效；
- 生成炮口特效。

RP 没有这个分支；`runAction` 也不会对地开火。因此：

- “Launch Nuke”按钮不会真的发射；
- 最多走一下动作冷却/动画；
- 因为 `price: ammo=1` 不是 credits，`a.price` 还是 0，连 ammo 也不会扣。

`[action_buildNuke]` 的 `addResources : ammo=1` 会被 `runAction` 的通用资源处理写入 `u.resources["ammo"]`，所以手动造弹可能有效；但 `aiUse : launchAmmo` 完全没被读取，AI 不会造/发核弹。反核弹拦截走的是另一套 `interceptProjectiles`，不受这个发射动作影响。

### 5.4 非 credits 价格会被当成 0

RP 的 `UnitAction` 定义只有一个 `float price`：

```cpp
float price = 0;
```

解析 `price : ammo=1` 时：

- `a.price` 仍是 0；
- `a.supported = false`；
- 资源检查/扣除不会发生。

实测有 8 个动作、6 个单位使用非 credits 价格：

- `experimental_carrier`、`experimental_gunship`、`modular_spider/antinuke`、`nautilus`、`nukes/antinuke_launcher` 的 `showAntiNukeCount`：`ammo=1`；
- `nukes/nuke_launcher` 的 `launchNuke`：`ammo=1`；
- `nautilus` 的 `launchScoutBot`：`credits=250`（这个是 credits，能正常解析）。

结果是这些动作在 RP 里**免费**，也不消耗对应资源。

### 5.5 `supported` 的检查范围不一致

`a.supported=false` 只影响：

- `actionEvent`（`Game.cpp:4292`）；
- 自动触发（`Game.cpp:4344`）。

手动 UI 和 `runAction` 并不检查 `supported`。所以按钮可能照常显示、照常扣冷却，但特殊效果什么也不做；这比“按钮直接隐藏”更容易让玩家误判。

---

## 6. 队伍、经济与胜负：硬编码覆盖地图数据

### 6.1 队伍关系

RP 的 `Team`（`Game.h:509-516`）只有：

```cpp
int id;
std::string name;
int color;
float credits;
bool isPlayer;
bool alive;
```

没有 `allyGroup`、没有外交关系、没有中立队伍语义。`isEnemy(a,b)` 就是 `a != b`。

因此：

- 所有非玩家队伍互相为敌；
- `allyGroup` 相同的 AI 不会结盟；
- 中立 team `-1` 在 `isEnemy` 里仍算敌人；树有 `cannotBeDirectlyAttacked` 保护，但其他中立单位不一定；
- 没有“队友视野共享”“队友不自动攻击”等规则。

### 6.2 起始 credits

RP 在 `Game.cpp:302-305` 和 `Game.cpp:344` 硬编码：

- 玩家：4000；
- 其他队伍：2500；
- 没有敌人的 fallback：3000。

地图 `team_info` 里实际的 credits 从 700 到 100000 都有，包括 10000、12000、37000、75000 等。也就是说，**绝大多数地图的起始经济都被改写了**。

1v3 Island 只是最明显的一例：玩家 4000 刚好对上，AI 的 3000/3000/3500 全变成 2500。

### 6.3 胜负

`checkWinLoss` 只看 team 0 和“所有非玩家队伍是否还有单位”，不读 `map_info.winCondition`。这直接导致：

- `mainBuilings` 图被改成全灭；
- `none` 图仍会结束；
- `requiredObjectives` 图没有目标判定；
- 没有“失去主基地但还有部队”这类中间状态。

### 6.4 单位上限

RP 没有 unit cap。`Game.cpp:2889` 的注释自己写着 “RP has no cap”；RWX 的单位上限、`ignoreInUnitCapCalculation` 等规则没有移植。大规模暴兵时，RP 的平衡与 RWX 不同。

### 6.5 开局基地

`Game::init` 在队伍没有任何单位时会硬塞一套开局：

- Command Center；
- Land Factory；
- 2 个 Builder；
- 非玩家队伍额外 4 个 Tank。

如果 AI 没有工厂，还会在基地附近免费补一座（`Game.cpp:347-377`）。这会掩盖“AI 不会造工厂”的问题，也会改变那些本来设计成“没有工厂/只有残部”的地图。

---

## 7. 寻路与移动：`OVER_CLIFF` / `HOVER` 规则反了

### 7.1 RP 的实现

`Game.cpp:529-535` 的 `passableAt`：

```cpp
case MoveType::AIR:
case MoveType::HOVER:          return map.inBounds(x, y);
case MoveType::WATER:          return map.walkableWater(x, y);
case MoveType::OVER_CLIFF_WATER: return map.walkableAmphibious(x, y);
default:                       return map.walkableLand(x, y);
```

`Pathfinding.cpp:13-20` 的 `isWalkable` 是同样的结构。

问题：

- **HOVER** 只检查 `inBounds`，等于可以穿过水、悬崖、大石头、**熔岩**，也可以穿过建筑占用格；RP 没有 `walkableHover`。
- **OVER_CLIFF** 没有独立 case，落到 `default` 的 `walkableLand`；而 `walkableLand` 会挡掉 `t.blocked`（悬崖/大物体）和水。于是“能越崖”的单位反而不能越崖。

### 7.2 RWX 的规则

`PathCostMap.java:320-343`：

- HOVER 可越 water / cliff / largeObject，但**熔岩恒阻挡**；
- OVER_CLIFF 可越 cliff / largeObject，但**不可越水**；
- OVER_CLIFF_WATER 可越 water / cliff / largeObject，只被熔岩和建筑占用阻挡。

RP 的 `walkableAmphibious` 对 OVER_CLIFF_WATER 是对的；错的是 HOVER 和 OVER_CLIFF。

### 7.3 出货单位

- 写 `movementType : OVER_CLIFF` 的 INI 有 **14 个**，其中 `mech_large_common`、`mech_med_common`、`common_slot`、`emptySlot` 是模板；实际单位包括 `bug_melee_t31`、`bug_ranged_t2`、`experimental_spider`、`mech_bunker`、`mech_heavyMissile`、`mech_engineer` 等。
- 写 `movementType : HOVER` 的 INI 有 **8 个**：`hovercraft`、`hovertank`、`heavyhovertank`、`experimentalhovertank`、`combat_engineer`、`experimental_gunship`、`nautilusSurface`、`scout`。

因为 `passableAt` 同时被 A* 和实际 `stepTo` 使用，这不是“路径选择差一点”，而是**移动规则本身错了**：悬浮单位能贴着熔岩/基地直线穿过，越崖单位却要绕路。

### 7.4 指令层缺失

- `orderMove` 每次都会 `path.clear()`（`Game.cpp:595-630`），没有 RWX 的 waypoint 队列；Shift 只用于控制组、回收选择等，不是“追加航点”。
- 没有 Patrol / Guard 动作。
- 没有 aggressive / outOfRange / onlyInRange 攻击模式。
- 没有“攻击地面”指令；`fireTurretXAtGround` 也没有实现。
- 寻路是同步单线程 A*，没有 RWX 的异步/流场/占位协商。

---

## 8. 迷雾：整个视野圆基本都算“可见”

### 8.1 公式差异

RWX `TileMap.java:1519-1580`：

- 内圈半径 `i - 3`，完全亮；
- 外圈按 `(d² - (i-3)²) * 10 / (i² - (i-3)²)` 从 0 渐变到 10；
- `>= 5` 就隐藏单位。

RP `Game.cpp:391-427`：

- 内圈 `i - 2`，比 RWX 多亮一圈；
- 外圈只取 `1..4`，封顶 4；
- `visibleToPlayer` 用 `< 5` 判断可见。

因为外圈永远到不了 5，**RP 在 `i` 半径内的所有格子都算可见**。

### 8.2 影响

- 内圈全亮半径多 1 tile；
- 有效可见半径比 RWX 大约 **1.2–1.5 tile**（sight=10 时约 10 vs 8.6）；
- RWX 的外圈是“半隐藏”的渐变，RP 的外圈只是纯视觉变暗，单位照样可见；
- RP 只有一张 `fog`，只给玩家算；AI 完全不受迷雾影响，`runAI` 直接扫描所有单位；
- `updateFog` 每 tick 把所有亮格降为 `5` 再重亮，没有 RWX 的 `fogOfWarCurrent/Next` 双缓冲和平滑维护；
- 地图 3 张 `fog=none`、1 张 `fog=los` 被忽略。

---

## 9. 生产：全额预付 vs 分批扣款

### 9.1 RP 的模型

`orderProduce`（`Game.cpp:838-844`）：

```cpp
if (!canAfford(f->team, tpl->price)) return false;
spend(f->team, tpl->price);
f->prodQueue.push_back(...);
```

- 下单瞬间扣全款；
- 取消时 `main.cpp:1656` 直接退 `u->price` 全款；
- 队列里的生产不会再检查钱，哪怕队伍 credits 已经归零也会继续造完。

### 9.2 RWX 的模型

`FactoryQueueManager.java:213-315` 按生产进度 `projectile.n` **分批扣 `UnitPrice`**：

- 钱不够就停产；
- 取消只退已经付掉的比例；
- 生产单位的成本可以来自 credits/energy/hp/shield/ammo/flag/自定义资源。

RP 没有 `streamingCost`，也没有 `UnitPrice` 的多资源模型；单位 `[core] price` 只取数字，动作价格也只存一个 float。

### 9.3 出厂锁定

RWX 新单位有 `spawnExitLockTimer = 30`：

- 普通工厂 30 tick；
- 实验厂 +40；
- 慢速单位再 +30/+20；
- 锁定期间不参与碰撞/移动。

RP 的 `spawnUnit` 放到出口后立即可动、可被挤、可参与碰撞。结合全款预付，RP 的爆兵节奏和 RWX 明显不同。

---

## 10. AI：仍是占位实现

`runAI`（`Game.cpp:3930-3965`）只做三件事：

1. 有工厂且队列空：从 `canBuild` 里挑**非建筑**单位生产；
2. 每 300 tick：让 builder 在 600px 内找资源格造 `extractor`；
3. 每 360 tick：如果非 builder 的战斗单位 ≥3，向 `nearestEnemyPos` 打一次 attack-move。

它**不会**：

- 造工厂、炮塔、防御、任何建筑（除了硬编码的 extractor）；
- 修理、回收、运输、装货、放技能、造/发核弹；
- 使用 `aiUse` / `aiTags`；
- 读取 43 个 `[ai]` 段、76 个 `[ai]` 键；
- 考虑迷雾（全图扫描）。

如果 AI 没有工厂，`Game::init` 会直接送它一座（`Game.cpp:347-377`），这也掩盖了“AI 不会造工厂”的问题。再叠加 `allyGroup` 被忽略，1v3 这类图会从“三家联合”变成“四家互殴”。

---

## 11. 表现层：drawLayer 没用，音频完全缺失

### 11.1 绘制层级

- `drawLayer` 只在 `Templates.cpp:1199` 解析、`UnitTemplate.h:789` 存着，**没有任何绘制代码读它**；`drawOrder` 根本没解析。
- RP 固定按“静态建筑 → 移动单位 → 弹道/FX”绘制（`Renderer.cpp:952-1020`），所有弹道都画在单位上方。
- RWX `GameLogic.java:1900-1995` 按 `drawLayer 0/其他/10` 分层，并用 `GameObjectComparator` 按 `drawLayer/drawOrder` 排序；地面弹道 layer 1 在单位前，空中弹道 layer 4 在单位后。

结果：地面炮弹/光束会盖在单位上面，空中单位和地面单位的叠放顺序也不稳定。

### 11.2 音频

- RP 源码里没有任何 `InitAudioDevice` / `LoadSound` / `PlaySound` / `PlayMusicStream`。
- `assets/sounds` 30 个、`music` 9 个都在，但完全没播。
- `--unusedkeys` 里仅 `[turret_*]` 的 `shoot_sound` / `shoot_sound_vol` 就有 150 条，`[effect_*]` 还有 61 条音效键。

### 11.3 UI 与资源

- RP 缺 RWX 的 `assets/ui`（42 个文件）和 `assets/shaders`（9 个）。
- 没有本地化：`i:gui.actions.launchNuke` 这类 text 会退化成英文 “Upgrade …”。
- 主菜单的 Watch Replay / Multiplayer 只是占位按钮（`main.cpp:135` 自己注释 “engine features not ported yet”）。
- 没有设置、统计、排行榜、聊天/标记。

---

## 12. 其他精确偏差

### 12.1 伤害公式顺序

RWX：

- `CustomUnit.setTarget:4187-4205` 先减护甲；
- `BaseUnit.setTarget:1560-1601` 再对 `buildProgress < 1` 的目标乘 1.75。

RP `Game.cpp:3430-3462`：

- 先乘 1.75；
- 再减护甲。

例：100 伤害、20 护甲、在建目标：

- RWX：`(100 - 20) * 1.75 = 140`
- RP：`100 * 1.75 - 20 = 155`

护甲越高差距越大。只影响在建/从废墟修复中的目标，但确实是数值偏差。

### 12.2 其他零散

- `showOnMinimap` 未读：所有单位都会上小地图（虽然受迷雾过滤）；
- `altNames` 未读：mod 用别名引用单位会失败；
- `[projectile_*]` 8 条、`[graphics]` 8 条键未读，主要是音效、sweep、绘制细节；
- 没有 unit cap；
- 中立 team `-1` 在 `isEnemy` 里仍算敌人。

---

## 13. 仍然成立的整块缺失

这些不是这次新发现，但仍是量级最大的差距：

| 系统 | 状态 |
|---|---|
| 音频 | 0 播放 |
| 联机 | 0 网络代码 |
| 存档/回放 | 0 序列化 |
| 任务/触发器 | 0 对象层解析 |
| JVM/Kotlin mod | 只支持 `.ini`/目录，不能跑 RWX 的 JVM mod API |
| UI/设置/统计/本地化 | 缺失或 stub |
| 地图类型/胜负/队伍外交 | 见第 3、6 节 |

`--unusedkeys` 的量化结果：

- **632 条未读键、115 个 INI**；
- `[core]` 210；
- `[turret_*]` 150（全是 `shoot_sound` / `shoot_sound_vol` 各 75）；
- `[ai]` 76；
- `[effect_*]` 61；
- `[projectile_*]` 8；
- `[graphics]` 8。

这个数字说明：资产内容对齐度很高，但 INI 解析覆盖度和完整 RWX 仍有明显距离。

---

## 14. 公平地说：战斗内核移植得很细

RP 不是草率重写。它把很多 RWX 的硬细节都逐行对上了：

- 多炮塔、warmup、aim、后坐、炮塔转向；
- 弹道物理、拦截、护盾/能量、溅射 falloff；
- 附件、运输、腿、动画、建造占地、修理/回收；
- 85/85 地图加载，261/261 drawable，220/220 tileset；
- `--checkmaps` 的 tile 层 0/85 需要修复；
- `--checkbuiltins` 159 个模板（其中 1 个 BADACT 是测试模板污染造成的）。

所以差距主要不在“战斗数学”，而在**游戏规则层和平台层**。

---

## 15. 优先级建议

| 优先级 | 项目 | 理由 |
|---|---|---|
| P0 | 解析 `Triggers` / `team_info` / `map_info` | 25+ 张图的胜负、29 个队伍的 credits/allyGroup、1v3 变 FFA，都是这一条 |
| P0 | 补 `fireTurretXAtGround` 等动作特效 | 8 个单位失去对地攻击，核弹发射无效 |
| P0 | 处理 `dont_load` / `NULL` | 模板污染、菜单出现 `null`、BADACT |
| P0 | 修正 `OVER_CLIFF` / `HOVER` | 移动规则反了，影响 14 + 8 个 INI |
| P1 | 迷雾公式 + AI 视野 | 玩家视野更大、AI 全图，直接改变对抗 |
| P1 | 生产分批扣款 / 出厂锁 | 经济节奏和 RWX 完全不同 |
| P1 | AI 的 `[ai]`、建筑、技能、运输 | 目前 AI 只会造兵和打一波 |
| P1 | 航点队列 / 巡逻 / 守卫 / 攻击模式 | 微操层缺失 |
| P2 | `drawLayer` / 音频 / UI | 表现层 |
| P2 | 存档 / 回放 / 联机 / 任务 / JVM mod | 平台能力 |

如果只想让 RP“更像 RWX 的对战”，先做 P0 的前四条；如果想做完整复刻，P2 的整块系统一个都绕不开。

---

## 16. 验证记录与限制

### 16.1 实际跑过的门

```text
rp.exe --unusedkeys   -> exit 0, 632 UNUSED, 115 INIs
rp.exe --checkmaps    -> exit 0, 0 of 85 map(s) need attention
rp.exe --checkbuiltins -> exit 0, 159 row(s), 1 bad reference(s)
```

### 16.2 静态统计

- 地图：85 张 `.tmx`；84 张有 `Triggers`；439 个 object；
- 对象：`map_info` 81、`team_*_info` 59、`allyGroup` 29、`winCondition` 17/8/1、`type` 55/13/8/5、`fog` 22/1/3；
- 移动：`OVER_CLIFF` 14 个 INI（含模板）、`HOVER` 8 个；
- 动作：`fireTurretXAtGround` 8 个 INI；非 credits 价格 8 个动作/6 个单位；
- `dont_load`：10 个文件，5 个有名字，3 个不该存在的模板；
- 资产：261 drawable、220 tileset、30 sound、9 music、42 UI、9 shader。

### 16.3 限制

- 没有做完整通关、动态对战或联网测试；
- `--checkmaps` 不校验对象层，所以不能把 `0/85` 当成地图语义正确的证据；
- `OVER_CLIFF` 的 14 个 INI 中包含模板文件，实际可玩单位少于 14；
- 部分 RWX 行号来自源码逐段核对的范围，具体行号可能随版本略有偏移；
- 没有做所有公式的逐行数值 diff，本文聚焦有可观察行为差异的规则。

---

## 附录 A：关键证据索引

| 结论 | RWX 证据 | RP 证据 |
|---|---|---|
| 地图对象层未解析 | 地图 `.tmx` 的 `Triggers` / `map_info` / `team_info` | `src/` 中 `objectgroup` 0 命中 |
| `dont_load` 被忽略 | `CustomUnitConfigParser.java:1184` | `Templates.cpp` 无检查；`--checkbuiltins` BADACT |
| 动作特效缺失 | `NukeLauncher.java:338-382` | `Templates.cpp:646-650`；`Game.cpp:4222-4281` |
| 非 credits 价格 | `UnitPrice.java` 多资源模型 | `UnitTemplate.h` 的 `UnitAction.price` 只有 float |
| 队伍关系/胜负 | 地图 `allyGroup` / `winCondition` | `Game.h:509-516`；`Game.cpp:3970-3976` |
| HOVER/OVER_CLIFF | `PathCostMap.java:320-343` | `Game.cpp:529-535`；`Pathfinding.cpp:13-20` |
| 迷雾公式 | `TileMap.java:1519-1580` | `Game.cpp:391-427` |
| 生产付款 | `FactoryQueueManager.java:213-315` | `Game.cpp:838-844`；`main.cpp:1656` |
| 出厂锁定 | `BaseUnit.java:171/829`；`CustomUnit.java:4433` | `Game.cpp` 的 `spawnUnit` 无锁 |
| 绘制层级 | `GameLogic.java:1900-1995` | `Templates.cpp:1199` 只解析；`Renderer.cpp:952-1020` |
| 伤害顺序 | `CustomUnit.java:4187-4205`；`BaseUnit.java:1560-1601` | `Game.cpp:3430-3462` |
| AI 占位 | `[ai]` / aiUse / AIController | `Game.cpp:3930-3965` |

## 附录 B：建议的落地顺序

1. **先修规则正确性**：地图对象层、队伍关系、credits、胜负条件、`dont_load`、动作特效、通行类型。
2. **再修对抗性**：迷雾、生产模型、AI 建筑/技能/运输、航点/巡逻/守卫/攻击模式。
3. **最后补平台能力**：音频、存档/回放、任务系统、联机、JVM mod、UI/设置/本地化。

这样每一步都能用现有门验证，不会一开始就陷入“大而全”的重写。

---

## 17. 修复记录（2026-11-21 晚）

按「先修规则正确性，再修对抗性」的顺序落地了 P0 的全部五条和 P1 的两条。每条都配了可复现的机器门，
新增的门用 **粗体** 标出。

### 17.1 `dont_load`：模板污染

`Templates.cpp::loadIni` 现在在解析前读 `[core] dont_load`（`CustomUnitConfigParser.java:1184`），
`canBuild_N_name` 也跳过 `NULL`（机甲工厂那个占位槽）。

| 指标 | 修复前 | 修复后 |
|---|---:|---:|
| 模板数 | 159 | **154** |
| `--checkbuiltins` BADACT | 1 | **0** |
| mechFactory 菜单里的 `null` | 有 | 无 |

`null` / `example_faction_picker` / `custom_tank` 三个不该存在的模板消失了（`NULL` 被三个文件各注册一次，
所以是 5 行）。

### 17.2 `HOVER` / `OVER_CLIFF` 通行规则

按 `PathCostMap.d:320-343` 的三个布尔（z 水 / z2 崖 / z3 大物体）重写：

| 移动类型 | 可越 | 被挡 |
|---|---|---|
| LAND | — | 水、崖、大物体、熔岩 |
| **HOVER** | 水、崖 | **大物体、熔岩**、建筑 |
| **OVER_CLIFF** | 崖、大物体 | **水**、熔岩、建筑 |
| OVER_CLIFF_WATER | 水、崖、大物体 | 熔岩、建筑 |
| WATER | 水/水桥 | 崖、大物体、熔岩 |

同时去掉了 `computePath` / `findPath` 里 HOVER 走直线的捷径——规则变严后它必须真的寻路。
**`--testpass`** 把这张表逐格套到整张地图上：火山图（lava=242、water=9485）与巨石图
（largeObject=2933）都是 0 failure。

### 17.3 伤害顺序

`CustomUnit.setTarget:4187-4205` 先减护甲，`BaseUnit.setTarget:1560-1562` 再乘在建 1.75。RP 原来是反的，
现在改成先护甲。`--testdamage` 新增一条合成用例（20 护甲 + 50% 建造进度 + 100 伤害）：**got=140.0**，
乘反了会是 155。

### 17.4 迷雾公式

`revealFogAt` 改成 `TileMap.updateFogVisibilityForTeamCircle:1543-1580` 的原式：内圈 `i-3`，
外圈 `(d²-(i-3)²)*10/(i²-(i-3)²)` 一直渐变到 **10**（原来内圈 `i-2`、外圈封顶 4，等于半径内全可见）。
sight=15 时可见半径从 15 tile 收到约 13.6。渲染端 alpha 也跟着从 150 爬到 255，保留渐变。
`--testfog` 新增断言：sight 15 时 13 tile 处必须可见、15 tile 处必须不可见。

### 17.5 地图对象层

`TileMap::loadTmx` 现在解析 `Triggers` 对象层，暴露 `MapInfo`（type / winCondition / loseCondition /
fog / introText）、`MapTeamInfo`（team / credits / allyGroup）和 `camera_start`。85 张图实测：
`Triggers` 层 84/85、`map_info` 81、`team_N_info` 59 个对象全部读到。

### 17.6 队伍、经济与胜负

- `Team::allyGroup` + `isEnemy()`：同组结盟，`-1`（地图没写）仍然互敌。`findEnemyUnits` /
  `nearestEnemyPos` 都走这个判定，所以 AI 也不会再打盟友。
- 初始 credits 取自 `team_info`（缺省才用原来的 4000/2500）。
- `checkWinLoss` 读 `map_info.winCondition`：`mainBuilings`（= `isBuilding && isFactory && 非采掘`）、
  `allBuildings`、`none`（永不判胜）、`requiredObjectives`（没有任务系统，按 `none` 处理而不是猜）。
- `fog=none` 的地图自动关雾；`camera_start` 成为开局相机位置。

**`--testteams`** 逐图检查：结盟对称性、同组必须非敌、开局不能已经判负。85 张图全过。

`1v3 Island` 现在读出来是 team 0（4000，组 0）对 team 1/2/4（3000/3000/3500，组 1），
即「三家联合挑战玩家」，不再是四家混战。

### 17.7 动作系统

- **非 credits 价格**：`price : ammo=1` 现在解析成 `ResourceCost` 并真的扣池子；`actionAvailable`
  会因为没有弹药而隐藏按钮。
- **`fireTurretXAtGround`**：新增 `Game::runActionAt(unitId, actionName, point)`——找到同名炮塔、
  先把炮口转向目标点（无制导炮弹会保持发射朝向）、再走 `Game::fire(u, nullptr, ti, point)`。
  核弹额外按 `NukeLauncher.a:338-382` 限制最小 450 px。HUD 上这类动作点击后进入「点地发射」模式
  （右键/Esc 取消）。
- **`deleteSelf`**：动作结束后 `removeUnit`（无爆炸、无残骸）。
- **`self.timeAlive(...)`**：条件原子补上 `timeAlive`，`Unit::age` 每 tick +1。10 秒信号弹
  因此能自己消失。
- **`resourceAmount` + `resourceAmount_setValue`**：这是「把池子设为某值」，不是 addResources
  的别名（AA beam gunship 的 afterburn 回填 energy=6）。

**`--testfire nukeLauncherC`**：空弹夹拒绝发射、100 px 拒绝、600 px 发射成功、ammo 5→4、弹道带
`nuke` 标记并朝目标飞（599→419 px，先爬到 posZ 110 再平飞）。
**`--testself flare_10s`**：599 tick 时自删。

### 17.8 仍然存在（未修）

- `requiredObjectives` 没有任务系统，按「不判胜」处理；
- `introText` 读到了但还没有简报 UI；
- AI 仍然不受迷雾限制、不会造建筑/技能/运输；
- 生产仍是全款预付、无出厂锁；
- `drawLayer` / 音频 / 存档 / 联机 / JVM mod 不动；
- `showOnMinimap` / `altNames` 未读；
- 中立 team `-1` 在 `isEnemy` 里仍算敌人。

### 17.9 回归

```text
--checkbuiltins                154 rows, 0 bad
--checkmaps                    0 of 85
--testbuild builder            placed + progress
--testrepair repairbay,landFactory   explicit=4 auto=40
--testweapon tank,c_tank       dmg=25
--testdamage c_tank            armour-order got=140.0
--testupgrade experimentalGunship ok=1
--listmods                     164 core ini / 154 templates
--testfog                      0 failure
--testgroups                   0 failure
--testfire nukeLauncherC       0 failure
--testself flare_10s           0 failure
--testteams (85 张图)          0 failure
--testpass (火山/巨石图)        0 failure
```

> 注意两个门需要正确的参数：`--testweapon tank,gunShip` 会失败（坦克打不到飞机，这是对的），
> `--testrepair builder,landFactory` 的 auto 半段会失败（vanilla builder 不自动修理，只有 repairbay 会）。
