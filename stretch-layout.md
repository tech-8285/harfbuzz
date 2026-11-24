# 伸展部件 → 轮廓 → 光栅化：离屏纹理的边界分析

场景：金额标签（伸展符号 + 一串数字）被绘进一张离屏纹理。阿拉伯数学排版中
伸展符号（stretchy operator）由字体的 MATH 表装配（assembly）给出：若干
**固定件**（如箭头头部）加若干**重复件**（extender，可重复 0..n 次）。装配后
的墨迹右沿可以越过标签的逻辑行宽，伸到后续数字上方。本文分析仓库中从伸展
部件到轮廓光栅化的实际代码，给出一组可复算的落点、纹理边界与行末笔位置，
并区分"由轮廓即可推出的边界"与"必须按像素覆盖规则才能判定的边缘"。

## 1. 代码路径

### 1.1 装配数据：MATH 表 → `hb_ot_math_get_glyph_assembly`

- 表结构：`src/hb-ot-math-table.hh`
  - `MathGlyphPartRecord`（`hb-ot-math-table.hh:765`）：`glyph`、
    `startConnectorLength`、`endConnectorLength`、`fullAdvance`、`partFlags`。
    `PartFlags::Extender = 0x0001`（`hb-ot-math-table.hh:756`）即"可跳过/可重复"
    的重复件标志。
  - `MathGlyphAssembly::get_parts`（`hb-ot-math-table.hh:855`）把部件记录换算
    成用户空间：`MathGlyphPartRecord::extract`（`hb-ot-math-table.hh:783`）用
    `font->em_mult (v, font->dir_mult (direction))` 缩放；`dir_mult`
    （`src/hb-font.hh:159`）对 LTR/RTL 都返回 `x_mult`，所以水平装配的数值
    与方向无关、恒为正。
  - `MathVariants::get_glyph_parts`（`hb-ot-math-table.hh:1141`）→
    `get_glyph_construction`（`hb-ot-math-table.hh:1155`）：按方向选
    vert/horiz coverage 查 construction；查不到时返回
    `Null (MathGlyphConstruction)`，装配数为 0（nil-object 惯例）。
  - `minConnectorOverlap`：`MathVariants::get_min_connector_overlap`，公开为
    `hb_ot_math_get_min_connector_overlap`。
- 公开 API：`hb_ot_math_get_glyph_assembly`（`src/hb-ot-math.cc:334`）、
  `hb_ot_math_get_glyph_variants`（`src/hb-ot-math.cc:270`）。
- **边界**：HarfBuzz 只暴露部件与连接子长度，**不做装配排版**。选几个
  extender、部件摆在哪、重叠多少，是调用方按 OpenType MATH 规范自行决定的
  （本文第 2 节给出参考算法）。仓库内的参考数据见
  `test/api/test-ot-math.c`（`MathTestFontFull.otf` 的 left/horizontal/right
  三件套）。

### 1.2 轮廓与墨盒：`hb_font_draw_glyph` / `hb_font_get_glyph_extents`

- 轮廓回调：`hb_font_draw_glyph`（`src/hb-font.cc:1720`）经 font funcs 走到
  glyf/CFF 的 path-builder，向 `hb_draw_funcs_t` 发
  move_to/line_to/quadratic_to/cubic_to/close_path（`src/hb-draw.cc`）。
- 墨盒：`hb_font_get_glyph_extents`（`src/hb-font.cc:1512`）→
  `hb_ot_get_glyph_extents`（`src/hb-ot-font.cc:811`），依次尝试
  sbix → CBDT → COLR → VARC → **glyf** → cff2 → cff1；glyf 分支
  （`src/OT/glyf/glyf.hh:542`）由轮廓点求边界盒。这是"由轮廓可推出"的保守
  墨盒：是控制盒/点盒，不保证曲线真的碰到盒边。

### 1.3 光栅化：`hb-raster-draw`（实验库 harfbuzz-raster，`-Dexperimental_api=true`）

- 累积：`hb_raster_draw_glyph_or_fail`（`src/hb-raster-draw.cc:1238`）把当前
  transform（`hb_raster_draw_set_transform`，含 dx/dy 平移）下的字形轮廓
  逐段 `emit_segment` 成规范化的边（y 单调、带 ±1 绕向），坐标为 24.8 定点
  （`HB_RASTER_PIXEL_BITS = 8`，`hb-raster-draw.cc:46`）。多次调用之间改
  transform，即可把多个部件轮廓累积进同一个 `hb_raster_draw_t`。
- 出图：`hb_raster_draw_render`（`src/hb-raster-draw.cc:1661`）：
  1. **自动边界**：未设 extents 时，取所有边的包围盒，min 向下、max 向上取整
     （`>> 8` 与 `+255 >> 8`），再被 `HB_RASTER_MAX_AUTO_DIMENSION = 4096`
     （`src/hb-limits.hh:239`）截断；stride 4 字节对齐。
  2. **固定边界**：`hb_raster_draw_set_extents` /
     `hb_raster_draw_set_glyph_extents`（`hb-raster-draw.cc:365/416`，后者把
     `hb_glyph_extents_t` 变换后 floor/ceil）直接指定输出窗口；此时窗外曲线
     在 flatten 阶段即塌缩成弦（`hb_raster_draw_update_flatten_clip`，
     `hb-raster-draw.cc:335`，注释说明该替换对渲染是精确的）。
  3. **扫描转换**：按行桶排边、活动边表扫描，`cell_add` 累积 area/cover
     （列号越界时被钳到图像内，`hb-raster-draw.cc:1297`），
     `sweep_row_to_alpha`（`hb-raster-draw.cc:1513`）按**非零环绕**把
     `|cover·2·ONE_PIXEL − area|` 钳到满覆盖后映射到 0..255。
  4. **尾部填充**：一行扫完若累计 cover 未抵消，则从最后触及的列到图像右沿
     `memset` 常量 alpha（`hb-raster-draw.cc:1840` 附近）。这意味着：被固定
     窗口**裁掉的墨迹不会消失，而是被"投影"成切口列上的实心 alpha**——
     第 2.4 节有实测。
  5. 预算护栏：`HB_BUDGET_RASTER_PIXELS` 等（render 前的 pixel-work 检查），
     防恶意轮廓撑爆内存/CPU。

## 2. 可复算实例

字体：`test/subset/data/fonts/STIXTwoMath-Regular.ttf`（upem 1000，含 MATH 表
与 ASCII 数字）。设备分辨率取 16 px/em，即 1 du = 0.016 px。复算程序与命令见
第 5 节；全部中间值均由仓库代码（hb-shape / hb_ot_math_* / hb-raster）产出。

### 2.1 金额与行末笔位置

`hb-shape`（scale = upem，输出即设计单位 du）：

| 金额 | 逐字 advance (du) | 行末笔 x |
|---|---|---|
| `$123.45` | 495·6 + 245 | **3215 du = 51.440 px** |
| `$9.99` | 495·3 + 245 | **2225 du = 35.600 px** |

行末笔位置 = 各 glyph `x_advance` 之和，这就是同一行下一段文字的起笔点
（LTR 下在原点右侧 3215 du 处）。RTL 对照：`test/api/fonts/Estedad-VF.ttf`
排阿拉伯-印度数字金额 `١٢٣٫٤٥`（16 px），得
`[uni0665=5+9|uni0664=4+8|uni066B=3+6|uni0663=2+11|uni0662=1+8|uni0661=0+5]`，
段宽 9+8+6+11+8+5 = **47 px**，视觉序自右向左，下一段文字在该段起点**左侧**
47 px 处起笔（RTL 下 x_advance 仍为正，方向由调用方应用）。

### 2.2 伸展符号的部件（U+2192 arrowright 水平装配）

`hb_ot_math_get_glyph_assembly (font, gid=1510, LTR, …)` 返回 2 个部件，
`italicsCorrection = 0`，`hb_ot_math_get_min_connector_overlap = 100`：

| 部件 | gid | start_conn | end_conn | full | 类型 | 墨盒 x | 墨盒 y |
|---|---|---|---|---|---|---|---|
| `horizontal.x` | 4814 | 250 | 250 | 597 | EXTENDER | [0,596] | [225,293] |
| `uni2192.var`（箭头） | 1511 | 0 | 200 | 505 | fixed | [0,448] | [58,460] |

装配布局（调用方算法，OpenType MATH 规范）：相邻部件重叠
`overlap = max(minConnectorOverlap, min(左件 end_conn, 右件 start_conn))`。
对本字体：ext→ext 重叠 min(250,250)=250；ext→fixed 重叠 min(250,0)=0 →
钳到 100。故每个 extender 的步进为 597−250 = **347 du**，k 个 extender 加
箭头头的装配推进为 **A(k) = 347k + 655 du**，取最小使 A ≥ T 的 k。

### 2.3 部件落点（T = 3215 du，金额 `$123.45`）

k = 8 个重复件 + 1 个固定件：

| part | gid | x (du) | x (px) | 逻辑覆盖 (du) |
|---|---|---|---|---|
| 0..7 | 4814 ext | 0, 347, 694, 1041, 1388, 1735, 2082, 2429 | 0 … 38.864 | [x, x+597) |
| 8 | 1511 fix | 2926 | 46.816 | [2926, 3431) |

- 装配推进 **A = 3431 du = 54.896 px**，溢出 **A−T = +216 du = +3.456 px**。
- 轮廓墨盒（各部件 extents 平移后取并）：x [0, 3374] du = [0, 53.984] px，
  y [58, 460] du → 纹理高度 floor/ceil 后 8 px。
- **墨迹右沿 3374 du 越过逻辑行宽 T=3215 du 共 159 du = 2.544 px**——这
  2.544 px 就是会画到后续数字上方的箭头墨迹。注意它小于装配推进的溢出
  （216 du），因为箭头头部右侧还有 57 du 的 side bearing。

### 2.4 纹理边界：自动 vs 按逻辑行宽裁切

同一批轮廓（9 个部件按上表落点累积进一个 `hb_raster_draw_t`）渲染两次：

| | 自动边界（轮廓推出） | 固定边界 = 逻辑行宽 51 px |
|---|---|---|
| 纹理 | origin (0,0)，**54×8** px | origin (0,0)，**51×8** px |
| 覆盖墨迹 | 实际墨列 x ∈ [0,53]；**6 个墨像素落在 x ≥ 52**（ceil(51.44)） | 切口列 x=50 上 **8 行全部带 alpha** |
| 含义 | 墨迹真实右沿到 x=53（= floor(53.984)） | 越界墨迹被钳进切口列：尾部填充把未抵消的 cover 抹成实心边 |

金额变为 `$9.99`（T = 2225 du = 35.6 px）时：k 降为 5，箭头头退到
x = 1885 du，A = 2390 du = 38.240 px（溢出 +165 du = +2.640 px），墨盒右沿
2333 du = 37.328 px；自动纹理 38×8，4 个墨像素 ≥ x=36；35 px 固定纹理在
x=34 列上有 6 行被裁墨迹。

**数字宽度每变一个数字（±495 du = ±7.92 px），k 至多变 1，装配推进以
347 du = 5.552 px 为步长跳动**；固定件（箭头头）的位置 = A(k) − 505，
随 k 逐档右移/左移，而不是随 T 连续移动。重复件的覆盖区间互相重叠 250 du
（4 px）：在同一张纹理里按非零环绕合成时重叠不可见（|cover| 钳顶，无接缝）；
若调用方把部件渲染成**多张小纹理再 alpha 合成**，重叠带会双倍变深——这属于
覆盖/合成规则问题，轮廓数据本身回答不了。

## 3. 两类边界的分界

**由轮廓即可推出（不需要光栅化）**：

- 每个部件的墨盒：`hb_font_get_glyph_extents`（glyf 点盒）。
- 装配推进 A(k)、部件落点、逻辑覆盖区间：纯由 full/connector/minConnectorOverlap 算出。
- 自动边界的纹理盒：轮廓边包围盒 floor/ceil（保守，必含全部墨迹）。
- 行末笔位置：shaping 的 advance 之和。

**必须按像素覆盖规则才能判定**：

- 墨迹真实右沿所在的**像素列**（曲线未必碰到墨盒边；本例墨盒 53.984 px，
  实墨到列 53）。
- 切口列上的 alpha 值：固定窗口渲染时越界 cover 被尾部填充投影到切口列
  （`hb-raster-draw.cc:1840` 附近），切口列呈现的是"右侧所有墨迹的剪影"，
  不是真实的部分覆盖。
- 连接子重叠带（本例 250 du = 4 px）在跨纹理合成时的颜色：取决于环绕规则
  与混合方式。
- 亚像素落点：部件 x 落点 46.816 px 这类非整数偏移下，边缘像素的覆盖率
  分布。

## 4. 给调用方的结论

1. **纹理宽度**：按逻辑行宽 T 分配会裁掉 A−T（本例 3.456 px）的装配溢出，
   且裁掉的墨迹会以实心边列的形式"印"在切口上。要完整保留伸展符号，纹理
   宽度至少取 ceil(A·s)（本例 55 px），或取轮廓墨盒并集 ceil(53.984)+1。
2. **相邻文字位置**：行末笔在 T 处，但墨迹到 T + 159 du。若下一段文字必须
   从 T 起笔，应把标签纹理裁在 [0, ceil(A·s)) 并在合成时允许越界部分覆盖
   到下一段纹理之上；否则把下一段起点推到墨迹右沿（3374 du）之后。
3. **重复件计数**：k = ⌈(T − 655)/347⌉（本字体），T 每增 347 du k 加 1；
   固定件位置只随 k 跳动，不要假设它随 T 连续移动。
4. **合成方式**：部件应累积进同一纹理（同一 `hb_raster_draw_t`）再渲染，
   让非零环绕规则吸收连接子重叠；分纹理合成需自行处理 4 px 重叠带。

## 5. 复算方法

```sh
meson setup build -Ddocs=disabled -Dbenchmark=disabled -Dexperimental_api=true
meson compile -C build

# 金额 advance 与 RTL 对照
build/util/hb-shape test/subset/data/fonts/STIXTwoMath-Regular.ttf '$123.45'
build/util/hb-shape --font-size=16 test/api/fonts/Estedad-VF.ttf '١٢٣٫٤٥'

# 部件落点 / 纹理边界 / 裁切实测（程序见下，保存于仓库外）
cc -o stretch_layout stretch_layout.c -I src -I build/src \
   -L build/src -lharfbuzz -lharfbuzz-raster -Wl,-rpath,$PWD/build/src
./stretch_layout   # 在仓库根目录运行
```

`stretch_layout.c` 做四件事：用 `hb_shape` 求两个金额的 advance；用
`hb_ot_math_get_glyph_assembly` 读 U+2192 的部件；按第 2.2 节算法摆放部件
并打印落点；把部件轮廓按落点平移累积进 `hb_raster_draw_t`，分别以自动边界
和"逻辑行宽"固定边界渲染，打印纹理盒、越界墨像素数与切口列上的行数。
本文全部数字即其输出。
