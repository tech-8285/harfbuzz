# 部分预组谚文字体下的音节等价性、声调点定位与断行边界

分析对象：本仓库 HarfBuzz（HEAD `cad7de9`，工作区 `0157-harfbuzz-6-deb4b00e`）。
所有结论均用仓库自带测试字体和当前构建的 `build/util/hb-shape` 实测复核。

---

## 0. 结论速览

1. 对带尾辅音（jongseong）的音节，例如 `한`：
   - 整字输入 `U+D55C`；
   - 字母输入 `U+1112 U+1161 U+11AB`（ㅎ + ㅏ + ㄴ）。

   Hangul shaper（`src/hb-ot-shaper-hangul.cc`）在任何 GSUB 之前，用
   `font->has_glyph()`（本质是查当前这一个 `hb_font_t` 的 cmap）决定：
   **能整字就交 1 个预组字形；不能整字但三个字母字形齐全，就交 3 个 jamo
   字形并打 `ljmo/vjmo/tjmo` 特性；覆盖有洞就产生 `.notdef`。**
2. 两种输入在以下条件下可见结果完全相同：
   - 字体有预组整字 `U+D55C`（字母输入会被算法式合成回该整字，此时甚至不要求
     cmap 里有三个 jamo）；或
   - 字体没有 `U+D55C`，但 `U+1112/U+1161/U+11AB` 三个字母字形都在（整字输入会被
     分解成同一组 jamo，并获得相同的特性掩码）。
   - 若整字与某个 jamo 同时缺失，两条路径的 `.notdef` 数量和宽度不同，可见结果分叉。
3. `U+302E`（单点声调号）和 `U+302F`（双点声调号）在 Unicode 里是 `Mc`、
   组合类 `ccc=224`、GCB=`Extend`、UAX#14 断行类 `CM`。HarfBuzz 是否把它
   **移到整个音节之前**，取决于该字体里该字符字形的**水平步进是否为 0**，而不是
   Unicode 类别：
   - 非零步近（“宽点”）：把声调字形 `memmove` 到音节字形之前，并合并 cluster；
   - 零步近：认为字体打算让它 overstrike，保留在音节之后，交给 GPOS 或 fallback
     定位把它挪到音节上方。
4. HarfBuzz **不做 UAX#14 断行判定**（没有 line-break API），它输出 cluster 和
   `HB_GLYPH_FLAG_UNSAFE_TO_BREAK`，由排版端结合 UAX#14/29 决定断行。对
   `가 + U+302F + 가`，声调字形带 `#1`（unsafe-to-break），后一个 `가` 不带标志：
   声调与前面的音节之间不能断开/独立重排，声调之后、下一个音节之前才是安全边界。
5. 必须区分三件事：**输出字形数组的顺序（屏幕先后）**、**`cluster` 字段（原文归属，
   真实 UTF-8 API 下是字节偏移）**、**glyph flags / cluster 边界（能否安全断开或
   拼接重排）**。宽点的字形顺序会变成“点在前、音节在后”，但它的 cluster 仍指向
   原文中位于音节之后的声调字符，且该边界是 unsafe。

---

## 1. 可复现的实验环境

```sh
meson setup build --reconfigure      # 若 build/ 已存在
meson compile -C build hb-shape
HB=build/util/hb-shape
TONE_FONT=test/shape/data/in-house/fonts/da410c694bf4bd186d0b114a5587d0d20df6766c.ttf
```

仓库里正好有一个为声调点准备的极简字体和一组成型期望：

- 字体：`test/shape/data/in-house/fonts/da410c694bf4bd186d0b114a5587d0d20df6766c.ttf`
- 用例：`test/shape/data/in-house/tests/hangul-jamo.tests:4-14`

该字体 `unitsPerEm=1000`，cmap 与度量如下（fontTools 实测）：

| Unicode | 字形名 | h-advance | glyf 包围盒 `(xMin,yMin,xMax,yMax)` |
|---|---|---:|---|
| `U+1100` | `kiyeok` | 500 | `(0,0,400,500)` |
| `U+AC00` | `ga` | 500 | `(0,0,400,500)` |
| `U+25CC` | `dottedcircle` | 500 | `(0,0,400,500)` |
| `U+302E` | `singleDot` | **200** | `(0,0,400,500)` |
| `U+302F` | `doubleDot` | **0** | `(0,0,400,500)` |

字体没有 `GSUB/GPOS`，所以偏移全部来自 HarfBuzz 的 fallback mark positioning，
最适合把算术讲清楚。下面所有引用该字体的输出都与 `hangul-jamo.tests` 中的期望逐字一致。

为了覆盖“整字/字母/覆盖有洞”矩阵，另用 fontTools 生成了几个度量受控的小字体
（见附录 A）。它们只用于把覆盖分支跑全；第 4 节的定位与边界演算只用上面这个
**仓库自带字体**，任何人都能直接复现。

---

## 2. 同一个带尾辅音音节：整字 vs 字母

### 2.1 两种输入在编码上的关系

HarfBuzz 使用经典的算法式谚文合成/分解常数
（`src/hb-ot-shaper-hangul.cc:110-119`）：

```
S = SBase + (l - LBase) * NCount + (v - VBase) * TCount + t
SBase=0xAC00  LBase=0x1100  VBase=0x1161  TBase=0x11A7
LCount=19     VCount=21     TCount=28     NCount=588
```

对 `한`：

- `ㅎ U+1112` → `l = 0x1112-0x1100 = 18`
- `ㅏ U+1161` → `v = 0`
- `ㄴ U+11AB` → `t = 0x11AB-0x11A7 = 4`

因此

```
S = 0xAC00 + 18*588 + 0*28 + 4 = 0xD55C
```

即 `U+D55C HANGUL SYLLABLE HAN`。两条输入路径是：

- 整字：`[U+D55C]`
- 字母：`[U+1112, U+1161, U+11AB]`（`<L,V,T>`）

### 2.2 Shaper 在不同 cmap 覆盖下交给字体什么

入口是 `preprocess_text_hangul()`（`hb-ot-shaper-hangul.cc:143`）。文件顶部的
大注释（`:150-196`）明确写了设计目标：

> If the whole syllable can be precomposed, do that. Otherwise, fully decompose
> and apply ljmo/vjmo/tjmo features.

覆盖判断全部经由 `font->has_glyph(cp)`；它只是
`get_nominal_glyph()` 的布尔包装，即查 cmap（`src/hb-font.hh:338-342`）。

**(a) 字母输入 `<L,V,T?>` —— 尝试合成整字**（`:263-317`）

```cpp
if (isCombiningL(l) && isCombiningV(v) && (t == 0 || isCombiningT(t))) {
  hb_codepoint_t s = SBase + ...;
  if (font->has_glyph (s)) {                 // 字体收了整字
    buffer->replace_glyphs (t ? 3 : 2, 1, &s);   // 3(或2) 个输入 -> 1 个 S
    ...
  }
}
/* 合成失败：保留各 jamo，分别打 LJMO/VJMO/TJMO */
buffer->cur().hangul_shaping_feature() = LJMO; ...
```

注意：合成只要求 `isCombiningL/V/T`，也就是现代谚文的 `U+11xx` 可组合子集
（`:121-123`）。扩展区/古谚文 jamo（`U+A960..A97C`、`U+D7B0..D7C6`、
`U+D7CB..D7FB` 等，见 `isL/isV/isT`，`:126-128`）不会被算法式合成。

**(b) 整字输入 `<LV>` 或 `<LVT>` —— 缺整字时尝试分解**（`:319-399`）

- 若 `S` 是无尾的 `LV` 且后面紧跟一个可组合 `T`，先尝试 `<LV,T> -> LVT`
  （`:329-344`）：`new_s = s + (T - TBase)`，字体有 `new_s` 就二合一。
- 否则，当“字体没有 `S`”或“`LV` 后面跟了（可能不可组合的）`T`”时，尝试分解
  成 `decomposed[3] = {LBase+lindex, VBase+vindex, TBase+tindex}`，但有一个
  **原子性的覆盖前提**（`:357-359`）：

```cpp
if (font->has_glyph (decomposed[0]) &&
    font->has_glyph (decomposed[1]) &&
    (!tindex || font->has_glyph (decomposed[2])))
```

  三个（LV 只需两个）jamo 字形必须**全部存在**才分解；否则保持 `U+D55C`
  不动，后续 cmap 映射不到就得到 gid 0 `.notdef`。

分解后同样给三个槽位打 `LJMO/VJMO/TJMO`（`:381-385`），与字母路径的掩码一致。

`ljmo/vjmo/tjmo` 是该 shaper 默认开启的特性
（`:49-64`），真正的 positional 字形由字体 GSUB 完成；shaper 结构体指定
normalization 模式为 `NONE`、`fallback_position = true`（`:441-457`）。

### 2.3 覆盖矩阵实测

用 fontTools 构造四个字体（upem 1000；度量：`S.han` advance 600，三个 jamo
各 250，缺省 `.notdef` 500），差别只在 cmap 覆盖：

| 字体 | `U+D55C` | `U+1112` ㅎ | `U+1161` ㅏ | `U+11AB` ㄴ |
|---|:-:|:-:|:-:|:-:|
| `full` | ✅ `S.han` | ✅ | ✅ | ✅ |
| `jamoonly` | ❌ | ✅ | ✅ | ✅ |
| `hole` | ❌ | ✅ | ✅ | ❌ |
| `syllableonly` | ✅ `S.han` | ❌ | ❌ | ❌ |

分别整形整字 `U+D55C` 和字母 `U+1112,U+1161,U+11AB`，默认 cluster 级别：

| 字体 | 整字输入得到 | 字母输入得到 | 可见是否相同 |
|---|---|---|---|
| `full` | `[S.han=0+600]` | `[S.han=0+600]` | ✅ 完全相同 |
| `jamoonly` | `[L.hieuh+250\|V.a+250\|T.nieun+250]` | 同左 | ✅ 完全相同 |
| `hole` | `[.notdef=0+500]` | `[L.hieuh+250\|V.a+250\|.notdef+500]` | ❌ 一个豆腐块 vs 两个字母加一个豆腐块 |
| `syllableonly` | `[S.han=0+600]` | `[S.han=0+600]` | ✅ 相同（合成只查整字 cmap，不查 jamo） |

`jamoonly` 下用 `-V`/`--cluster-level=2 --show-flags` 能看到两条路径的
**原文归属**不同，但字形与位置一致：

```text
整字 U+D55C           : [L.hieuh=0+250|V.a=0+250|T.nieun=0+250]
字母 L,V,T (level 2)  : [L.hieuh=0+250|V.a=1+250#1|T.nieun=2+250#1]
```

整字分解出的三个字形 cluster 全是 `0`（都归属同一个原文字符）；字母输入在
`HB_BUFFER_CLUSTER_LEVEL_CHARACTERS(=2)` 下保留 `0/1/2`。默认 level 0
（MONOTONE_GRAPHEMES）会按字素合并，于是两行连 cluster 显示都一样。

### 2.4 `가 + ㄴ`：同一个 간 的三种写法

`간 U+AC04 = U+AC00 + tindex4`，所以它有三种合法输入：

1. 整字：`U+AC04`
2. 半组合 `<LV,T>`：`U+AC00 U+11AB`（가 + ㄴ）
3. 全字母 `<L,V,T>`：`U+1100 U+1161 U+11AB`

在 `full` 字体（有 `U+AC04`）下，三者都收敛成同一个 `[S.gan=0+500]`：

```text
U+AC04                 -> [S.gan=0+500]
U+AC00,U+11AB          -> [S.gan=0+500]
U+1100,U+1161,U+11AB   -> [S.gan=0+500]
```

在没有任何预组音节、但 jamo 齐全的 `jamoonly` 下，三者都收敛成
`[L.giyeok|V.a|T.nieun]`（cluster/flag 随写法不同）。这正是
`hb-ot-shaper-hangul.cc:329-389` 两条 `<LV,T>` 分支保证的等价性。

仓库里另一个回归字体
`test/shape/data/in-house/fonts/30a951cae79125d340a32761e2d710ed4c62cdc6.ttf`
演示了“`LV` 后面是**不可组合** T”的情况：它的 cmap 只有
`U+AC00(ga)`、`U+1100 L`、`U+1161 V`、`U+11EB T`，并带 `ljmo/vjmo/tjmo`
GSUB。`U+AC00,U+11EB` 无法合成预组字（`U+11EB` 超出 `isCombiningT` 范围），
于是 `AC00` 被分解并把该 T 一起纳入音节，输出
`[L.alt|V.alt|T.alt]`，对应 `hangul-jamo.tests:16-23` 的 cluster/`#3` 断言。

### 2.5 “两种输入得到相同可见音节”的充要条件（在单个 `hb_font_t` 内）

满足以下任一条即可：

1. **整字覆盖**：cmap 含目标预组音节 `S`。字母路径在
   `hb-ot-shaper-hangul.cc:287` 合成到它；整字路径直接保留它。
   不要求 cmap 含对应 jamo（见 `syllableonly` 实验）。
2. **完整 jamo 覆盖**：cmap 不含 `S`，但含规范分解所需的全部 jamo
   （LVT 要 L/V/T，LV 只要 L/V）。整字路径在 `:357-362` 原子地分解；
   字母路径在合成失败后保留同样的 jamo。两者拿到相同的
   `LJMO/VJMO/TJMO` 掩码（`:300-309` 与 `:381-385`），因此同一字体的
   GSUB/GPOS 会给出相同的可见结果。

会破坏等价性的情况：

- **覆盖有洞**：`S` 缺、且至少一个必需 jamo 也缺。整字路径拒绝部分分解，
  整个 `S` 变成一个 `.notdef`；字母路径让缺失的那个 jamo 变 `.notdef`，
  其余字母仍可见（`hole` 行）。
- **非现代可组合 jamo**：扩展区/古谚文不能算法式合成成 `S`，只能走 jamo
  特性路径；此时“整字 vs 字母”的前提本身就不成立。
- **字体行为差异**：若字体的 `ljmo/vjmo/tjmo`/GPOS 对“分解出来的 jamo”和
  “用户直接输入的 jamo”一视同仁（掩码相同，正常字体都如此），结果一致；
  HarfBuzz 层面两条路径的掩码确实相同。

排版端还要注意 **font fallback 是调用方的职责**。`hb_shape(font, ...)` 只面对
一个 `hb_font_t`；CoreText/DirectWrite/fontconfig 若按字符把缺字分派到不同后备
字体，整字路径可能整体落到字体 A，而分解后的 jamo 分别落到字体 B/C，此时
HarfBuzz 内的覆盖等价性不再保证最终像素一致。要获得本文的保证，排版端应把
同一个（已选定的）字体交给 HarfBuzz，或自行保证后备字体的覆盖连续。

---

## 3. 声调标记相对整音节的位置

### 3.1 字符属性：它是“间距组合记号”，零宽与否看字体度量

Python/Unicode 与最新 UCD 表（`LineBreak.txt`、
`GraphemeBreakProperty.txt`，Unicode 最新版）：

```text
U+302E HANGUL SINGLE DOT TONE MARK : gc=Mc  ccc=224  GCB=Extend  lb=CM
U+302F HANGUL DOUBLE DOT TONE MARK : gc=Mc  ccc=224  GCB=Extend  lb=CM
```

`ccc=224` 是 `HB_UNICODE_COMBINING_CLASS_LEFT`。HarfBuzz 的 mark 判定
`HB_UNICODE_GENERAL_CATEGORY_IS_MARK` 包含 `Mc`
（`src/hb-ot-layout.hh:290-294`），所以它会被 fallback 定位当记号处理。

但 shaper 决定“前移”还是“原地 overstrike”，用的是
`is_zero_width_char()`（`hb-ot-shaper-hangul.cc:135-141`）：

```cpp
return hb_font_get_glyph (font, unicode, 0, &glyph) &&
       hb_font_get_glyph_h_advance (font, glyph) == 0;
```

也就是说，**判据是该字体里声调字形的实际 h-advance**。同一个码位在不同字体里
可能走不同分支。仓库测试字体恰好把 `singleDot` 做成 200（宽）、`doubleDot`
做成 0（零宽），于是两个分支都被锁定为回归期望。

### 3.2 Shaper 的声调处理（`:208-257`）

只有当“刚处理完一个合法音节”（`start < end && end == out_len`）时才进入重排：

```cpp
buffer->unsafe_to_break_from_outbuffer (start, buffer->idx);
buffer->next_glyph ();
if (!is_zero_width_char (font, u)) {            // 宽点
  buffer->merge_out_clusters (start, end + 1);
  hb_glyph_info_t tone = info[end];
  memmove (&info[start + 1], &info[start],
           (end - start) * sizeof (...));       // 音节整体后移一格
  info[start] = tone;                           // 声调放到整个音节之前
}
```

- 宽点：声调和整个音节的 cluster 被合并，字形顺序变为 `[tone, S...]`。
- 零宽点：**不移动、不合并**，保留 `[S..., tone]`，注释假设它
  “designed to overstrike”。
- 没有合法音节作底座时，走 dotted circle 分支（`:229-254`）：字体有
  `U+25CC` 就插一个虚圆；宽点顺序为 `[tone, dottedcircle]`（宽点在前），
  零宽点顺序为 `[dottedcircle, tone]`。

### 3.3 具体度量演算：`U+AC00 U+302F`（零宽双点）

实测：

```sh
$HB --font-file=$TONE_FONT --unicodes U+AC00,U+302F --show-extents
[ga=0+500<0,500,400,-500>|doubleDot=0@-450,0+0<0,500,400,-500>]
```

输出语法是 `名字=cluster@x_offset,y_offset+x_advance`，尖括号是
`<x_bearing,y_bearing,width,height>`。

该字体没有 GPOS，且无 GPOS/kerx 时才启用 fallback mark positioning
（`src/hb-ot-shape.cc:194-203`）。调用链为
`_hb_ot_shape_fallback_mark_position` → `position_cluster` →
`position_around_base` → `position_mark`
（`src/hb-ot-shape-fallback.cc`）。

已知度量（1000 upem，未缩放时数值相同）：

- base `ga`：h-advance `500`；glyf 框 `xMin=0,xMax=400`，原始 extents
  `x_bearing=0, width=400`。但定位前 base 的 X 度量被改写为
  （`:349-350`）：
  - `base_extents.x_bearing = pos[base].x_offset = 0`
  - `base_extents.width = h_advance(base) = 500`（用步进，不用墨迹宽）
- mark `doubleDot`：`mark_extents.x_bearing = xMin = 0`，
  `mark_extents.width = xMax-xMin = 400`。
- `ccc=224 (LEFT)` 在 `position_mark()` 的 X switch 中没有专门分支，落入
  `default` 的“居中”公式（`:246-255`）：

```
x_align = base_x_bearing + (base_width - mark_width)/2 - mark_x_bearing
        = 0 + (500 - 400)/2 - 0
        = +50
```

LTR 下还有一个“笔尖已经走过 base 步进”的修正量
`x_offset = -pos[base].x_advance = -500`（`:358-360`），并在
`:409-412` 把记号步进清零、叠加该修正：

```
doubleDot.x_offset = x_align + (-500) = 50 - 500 = -450
doubleDot.x_advance = 0
```

这正是输出里的 `@-450,0+0`。换成绝对坐标（pen 原点）：

| 字形 | pen x | x_offset | 绘制原点 x | 墨迹区间 |
|---|---:|---:|---:|---|
| `ga` | 0 | 0 | 0 | `0..400` |
| `doubleDot` | 500（ga 步进之后） | −450 | **50** | `50..450` |

双点墨迹中心 `250` 正好是 `ga` 步进区间 `[0,500]` 的中心。也就是说，
**`-450` 不是“往左挪 450”这么简单，它 = 居中修正 `+50` 减去笔尖步进 `500`**。

`y_offset=0`：`ccc=224` 在 Y switch（`:274-321`）里同样没有对应分支，
fallback 不做垂直移动；真实字体里垂直位置通常由字形设计或 GPOS 锚点决定。

### 3.4 宽点 `U+302E`：移到整个音节之前，占自己的步进

```sh
$HB --font-file=$TONE_FONT --unicodes U+AC00,U+302E
[singleDot=0+200|ga=0+500]          # 默认 cluster 级别
```

因为 `singleDot` advance=200 ≠ 0，shaper 把它 `memmove` 到 `ga` 前面，
且**不会**被 fallback 重新定位：此时输出顺序是 `[singleDot, ga]`，
fallback 按“非记号字符”切分 cluster 时，`singleDot` 前面没有 base，
两个长度为 1 的片段都不会进入 `position_around_base`。

布局几何（输出顺序即 LTR 屏幕顺序）：

| 输出槽 | 字形 | pen x | advance | 说明 |
|---:|---|---:|---:|---|
| 0 | `singleDot` | 0 | 200 | 声调占独立的 200 宽，位于音节**前方（左侧）** |
| 1 | `ga` | 200 | 500 | 整音节从 x=200 开始 |

总宽 700。测试字体的方块轮廓会与 `ga` 在墨迹上相交，那只是占位轮廓；
真实字体靠 side bearing/轮廓留白避免重叠。

### 3.5 “相对整音节”还是“相对尾辅音”：整字与分解路径的差别

fallback 只会把记号挂到它前面“连续记号段”所跟随的**那一个非记号字符**上
（`position_cluster_impl()`，`:425-453`）。而 jamo `L/V/T` 的类别都是 `Lo`
（字母），不是记号。因此：

- 若音节以**单个预组字形**存在（如 `ga`/`S.han`），零宽点居中到**整个音节**
  的步进框上（3.3）。
- 若音节被**分解成 `[L,V,T]` 三个字形**，零宽点在字形流里位于 T 之后，
  fallback 会把它挂到 **T（尾辅音）** 上，而不是整个三字母音节。

用附录 A 的 `full/jamoonly` 字体（`S.han` advance 600、点墨迹
`xMin=100,xMax=300` 即 `x_bearing=100,width=200`；T advance 250）实测：

```text
U+D55C,U+302F (整字)
  -> [S.han=0+600 | doubleDot=1@-500,0+0#1]
     居中: 0+(600-200)/2-100 = +100;  100-600 = -500 ✓

U+1112,U+1161,U+11AB,U+302F (分解)
  -> [L.hieuh+250 | V.a+250#1 | T.nieun+250#1 | doubleDot=3@-325,0+0#1]
     对 T: 0+(250-200)/2-100 = -75;  -75-250 = -325 ✓
```

排版端在没有 GPOS 的字体上应当预期到这种差异；带有 `mark`/mkmk 定位或专门
声调锚点的真实字体由 GPOS 决定位置（此时 fallback 整体不启用），不受上述
公式约束。HarfBuzz 官方测试只锁定了“单个预组音节 + 点”的数值
（`hangul-jamo.tests:8-9`），没有对分解三 jamo + 点做断言。

### 3.6 没有合法音节时的虚圆

仓库字体实测，与 `hangul-jamo.tests:4-7,13-14` 一致：

```text
U+302E            -> [singleDot=0+200|dottedcircle=0+500]   # 宽点在虚圆前
U+302F            -> [dottedcircle=0+500|doubleDot=0@-450,0+0]
U+1100,U+302E     -> [kiyeok+500|singleDot@-450,0+0|dottedcircle+500]
```

最后一行是个值得注意的副作用： lone `U+1100` 不构成合法音节（后面不是 V），
shaper 仍插入虚圆；输出流变成 `[kiyeok, singleDot, dottedcircle]`，fallback
就把夹在中间的宽点当成 `kiyeok` 的记号，给出 `@-450,+0`（与 3.3 同一公式）。
这里的点在**原文归属**上没有可依附的音节，几何上却被 fallback 挂到了前面的
字母——排版端不应把这个几何附着误读成语音学归属。

---

## 4. 断行判断：屏幕顺序、原文归属、独立重排边界

### 4.1 分工：UAX#14 决定“有没有断行机会”，HarfBuzz 决定“这个断点安不安全”

HarfBuzz 不提供 UAX#14 行断行类 API（`src/hb-unicode.h` 中搜不到
line-break 接口），它是整形库，不是断行库。排版端的正确流程是：

1. 用 UAX#14（ICU/Pango 等）在**源字符串**上枚举断行机会；用 UAX#29 字素
   边界处理 cluster 编辑单元。
2. 用 HarfBuzz 整形结果里的 `cluster` 把源位置映射到字形，并读取
   `HB_GLYPH_FLAG_UNSAFE_TO_BREAK`（`hb-buffer.h:76` 起的文档）：
   - 带 `0x1`：若在“该字形所属 cluster 的起点”处断开，断开后两侧必须重新
     整形，因为拼接结果可能变化；
   - 不带标志：可以在该 cluster 起点安全断行，两侧分别整形与整体整形结果
     逐字一致。

UCD 属性给出的语义边界：

- `U+302E/U+302F` 是 `Line_Break=CM`、`GCB=Extend`。
- UAX#14 **LB9**：“Do not break a combining character sequence; treat it as
  if it had the line breaking class of the base character …”——声调点与底座
  之间禁止断开，`CM` 继承底座的断行类。
- 谚文相关 **LB26** 用三条禁止链保持“谚文音节块”：

```text
JL × (JL | JV | H2 | H3)
(JV | H2) × (JV | JT)
(JT | H3) × JT
```

  其中 `U+AC00` 是 `H2`（LV），`U+D55C/U+AC04` 这类 LVT 是 `H3`，
  `U+1100..115F` 是 `JL`，另有 `JV/JT` 区间。两个相邻预组音节 `H2/H3` 之间
  并没有 `×` 禁止；再结合 LB9 让 `CM` 继承 `H2/H3`，所以
  `가 + 声调 + 가` 的结论是：**声调与前一音节之间不可断；声调之后、下一个
  音节之前是候选断点**（韩文排版常按空格/어절 进一步剪裁，那是上层策略）。

### 4.2 一组具体输入与三种坐标

用仓库字体即可复现的样本：

```text
原文：가 〯 가  =  U+AC00 U+302F U+AC00     （零宽双点）
      가 〮 가  =  U+AC00 U+302E U+AC00     （宽单点）
UTF-8：每个字符恰好 3 字节，字节偏移为 0 / 3 / 6
```

为避免 CLI 默认把 cluster 重编号成字符序号
（`util/shape-options.hh:126-136`），下面用一个直接调 C API
（`hb_buffer_add_utf8` + `HB_BUFFER_CLUSTER_LEVEL_CHARACTERS`）的探针取数；
等价 CLI 需加 `--utf8-clusters`：

```sh
$HB --font-file=$TONE_FONT --cluster-level=2 --show-flags --utf8-clusters \
    "가〯가"
[ga=0+500|doubleDot=3@-450,0+0#1|ga=6+500]

$HB --font-file=$TONE_FONT --cluster-level=2 --show-flags --utf8-clusters \
    "가〮가"
[singleDot=3+200#1|ga=0+500|ga=6+500]
```

C API 探针的逐字段结果（字节 cluster，flags 为实测位）：

| 样本 | 输出槽 | 字形 | `cluster`（UTF-8 字节） | flags | x_advance | x_offset |
|---|---:|---|---:|---:|---:|---:|
| 零宽 `〯` | 0 | `ga` | **0**（가） | 0 | 500 | 0 |
| | 1 | `doubleDot` | **3**（〯本身） | **1 UNSAFE_TO_BREAK** | 0 | −450 |
| | 2 | `ga` | **6**（第二个 가） | 0 | 500 | 0 |
| 宽 `〮` | 0 | `singleDot` | **3**（〮本身） | **1 UNSAFE_TO_BREAK** | 200 | 0 |
| | 1 | `ga` | **0**（가） | 0 | 500 | 0 |
| | 2 | `ga` | **6** | 0 | 500 | 0 |

这张表把三个概念彻底分开：

#### (1) 字形在屏幕上的先后 = 输出数组顺序 + advances/offsets

- 零宽点：`[ga, doubleDot, ga]`，顺序与原文一致；点靠 `x_offset=-450`
  叠到第一个 `ga` 上方，不占新步进。
- 宽点：`[singleDot, ga, ga]`，**前两个字形相对原文交换了顺序**；点以 +200
  步进排在第一个 `ga` 左边。LTR 下数组顺序就是视觉顺序。

#### (2) 对原文的归属 = `cluster` 字段

- 宽点虽然是输出第 0 个字形，`cluster=3` 却表明它来自原文第 2 个字符
  （字节偏移 3，即 `U+302E`，在 `가` 之后）；`ga` 的 `cluster=0`。
- 因此 level 2 下 cluster 序列可以是非单调的 `3,0,6`。这是合法输出，
  `HB_BUFFER_CLUSTER_LEVEL_CHARACTERS` 的文档明确允许“clusters might appear
  in any order”（`src/hb-buffer.h:441-446`）。
- `--unicodes U+...` 测试格式里显示的 `0,1,2` 是码位序号；真实
  `hb_buffer_add_utf8` 下是 **UTF-8 字节偏移 0/3/6**。选型时不要混淆。

#### (3) 允许独立重排/断开的边界 = flags + cluster 区间

- `doubleDot/singleDot` 上的 `#1` 表示“在 cluster 3 起点断开”是
  **unsafe-to-break**：这个位置正是源文里 `가 | 〯/〮` 之间。UAX#14 LB9
  同样禁止它。shaper 在重排前就调用了
  `unsafe_to_break_from_outbuffer(start, idx)`
  （`hb-ot-shaper-hangul.cc:218`），而通用字素聚类
  `hb_form_clusters()`（`src/hb-ot-shape.cc:580-587`）和 fallback
  `position_around_base()` 的 `unsafe_to_break(base,end)`
  （`hb-ot-shape-fallback.cc:334`）也会在该范围置位。
- 第二个 `ga`（cluster 6）flags=0：在 cluster 6 起点（声调之后、下一音节
  之前）断开是安全的。把前缀 `U+AC00,U+302E` 单独整形，得到
  `[singleDot=1+200#1|ga=0+500]`，与整段输出的前两个字形逐字一致，验证了
  “可独立整形/重排”的含义。
- HarfBuzz 内部允许做字形搬移的窗口，恰好就是代码记录的
  `start..end`（音节范围）再加声调槽 `end+1`；相邻音节不在窗口内，不会被
  这次 memmove 波及。排版端可以把这个“音节+声调”块当作不可拆分、可整体
  放置的单元；块内字形顺序由 HarfBuzz 负责，块间才是排版端的断行自由。

### 4.3 两个容易踩的几何/级别陷阱

**陷阱 A：不能按 pen x 坐标找断点。** 零宽样本中：

```text
ga pen=0 adv500 ; doubleDot pen=500 adv0 offset-450 ; 第二个 ga pen=500 adv500
```

“unsafe 的声调左缘（源边界 0|3）”和“safe 的第二音节起点（源边界 3|6）”
在几何上是**同一个 pen 坐标 x=500**（因为点步进为 0）。只看 x 无法区分这两个
语义完全不同的边界，必须回到源字符串的 UAX#14 机会并按 `cluster` 查 flags。

宽点样本同理：视觉上“点与第一个 ga 的交界”在 x=200，但它对应的是源文
`가|〮` 这个 unsafe 边界；真正安全的块间断点在第二个 `ga` 的 pen x=700。
另外，宽点字形自身的视觉左缘 x=0 带着 `cluster=3,#1`，它**不是**一个可以
直接落行首的、由源文断点推出的安全位置。

**陷阱 B：默认 cluster 级别下 `#1` 可能消失。** 同一输入在不同
`--cluster-level` 下实测：

```text
가〮가 (宽点):
  level 0 : [singleDot=0+200|ga=0+500|ga=2+500]      # 无 #，cluster 被合并
  level 1 : [singleDot=0+200|ga=0+500|ga=2+500]      # 单调化合并，# 被清
  level 2 : [singleDot=1+200#1|ga=0+500|ga=2+500]    # 无损：非单调 cluster + flag

가〯가 (零宽点):
  level 0 : [ga=0+500|doubleDot=0@-450,0+0|ga=2+500] # 字素合并，# 被清
  level 1 : [ga=0+500|doubleDot=1@-450,0+0#1|ga=2+500]
  level 2 : 同 level 1
```

原因在 `set_cluster()`（`src/hb-buffer.hh:654-660`）：每当 cluster 值被改写，
该字形低位的所有 defined glyph flags 会先被清掉；而
`merge_out_clusters()` 只在单调级别（0/1）真正合并
（`hb-buffer.hh:419-426`）。再加上整形前的 `hb_form_clusters()` 在非字素
级别用 `unsafe_to_break()` 代替合并（`hb-buffer.hh:407-417`），整形后的
`hb_propagate_flags()` 只在最终同 cluster 内做 OR 传播
（`hb-ot-shape.cc:1110-1167,1198`）。于是：

- **level 0（默认）**：声调和音节共享 cluster 0，`#1` 也在合并中被清。
  “不可拆”由“它们是同一个多码位 cluster/字素”表达——这个 cluster 内部本来就
  没有可供排版端使用的字符级断点（UAX#14 LB9 也禁止）。
- **level 1（推荐新程序使用，单调字符 cluster）**：零宽点保留独立 cluster+`#1`；
  宽点因重排破坏单调性，被强制并回 cluster 0 且清掉 `#1`。
- **level 2（CHARACTERS，最细且允许非单调）**：flag 与真实字符 cluster 都
  完整保留，是调试“屏幕顺序 vs 原文归属”的最佳级别，代价是 cluster 可能乱序。
- **level 3（GRAPHEMES，只合并字素、不强制单调）**：cluster 归并但不单调。

此外 `#3 = UNSAFE_TO_BREAK(1)|UNSAFE_TO_CONCAT(2)` 中的 bit2 需要显式打开
`HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT`（CLI 加 `--unsafe-to-concat`）才
可靠产生；`hangul-jamo.tests:16-23` 锁定了该行为。普通断行只需要 bit1。

### 4.4 给排版端的落地建议

1. buffer 设 `HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS(1)` 或
   `CHARACTERS(2)`；涉及重排声调且需要无损映射时用 2，并准备好处理非单调
   cluster。UTF-8 接入时 cluster 是字节偏移。
2. 断行机会以 UAX#14 为准：不要在 `CM`（声调点）与底座之间断行；谚文 jamo
   块按 LB26 保持。HarfBuzz 的 `UNSAFE_TO_BREAK` 用来校验所选断点是否需要
   重新整形，而不是反过来从字形 x 坐标猜断点。
3. 选中断点后，从该 cluster 边界到行尾重新整形；若起点字形带
   `UNSAFE_TO_BREAK`，按 `hb-buffer.h:96-143` 的说明向前回退到上一个安全
   cluster 再整段重形。安全边界上可以直接拼接两侧结果。
4. 声调和“音节块”作为一个整体摆放：宽点已被 HarfBuzz 排到音节字形之前并占
   步进；零宽点靠 offset 叠加且步进为 0，排版光标/选中矩形要把它的
   `x_offset` 和 0 advance 考虑进去，不能按 advance 累加命中测试。
5. 字体覆盖是单字体语义；若排版端自己做字符级 font fallback，需要在 fallback
   之后重新评估第 2.5 节的等价性与声调定位。

---

## 5. 一句话总括

- **覆盖**：HarfBuzz 只认当前字体的 cmap——有整字交整字，没整字但 jamo 齐全
  交三个带 `jmo` 特性的 jamo，有洞交 `.notdef`；两条输入在“整字存在”或
  “jamo 全覆盖”时得到同一个可见音节。
- **声调**：`U+302E/F` 是 `Mc/ccc224/GCB=Extend/LB=CM`；宽步进点被移到整个
  音节之前，零步进点留在后面由 GPOS/fallback 居中叠加。仓库字体的
  `doubleDot@-450 = (500-400)/2 - 500` 是最直接的度量样本。
- **边界**：字形顺序可以与原文顺序相反（宽点 `cluster=3` 排在 `cluster=0`
  之前），cluster 负责原文归属，`UNSAFE_TO_BREAK` 与 UAX#14 LB9/LB26 共同
  标出不可拆、不可独立重排的“音节+声调”块；块外（声调之后、下一音节之前）
  才是安全的断行与重排边界。

---

## 附录 A：覆盖矩阵受控字体的构造要点

第 2.3、3.5 节的实验字体由 fontTools 生成于本次分析的临时目录
（`/tmp/hb-hangul-demo/make_fonts.py`，不属于仓库）。核心度量与 cmap 如下，
可用等价代码重建：

```python
# upem=1000；FontBuilder + TTGlyphPen 画矩形轮廓
glyphs = {
  ".notdef":    (500, (0, 0, 400, 500)),
  "L.hieuh":    (250, (0, 0, 200, 500)),   # U+1112
  "V.a":        (250, (0, 0, 200, 500)),   # U+1161
  "T.nieun":    (250, (0, 0, 200, 500)),   # U+11AB
  "S.han":      (600, (100, 0, 500, 500)), # U+D55C
  "singleDot":  (200, (0, 0, 400, 500)),   # U+302E
  "doubleDot":  (0,   (100, 400, 300, 500)),# U+302F, mark 墨迹宽 200
  "dottedcircle":(500,(50, 50, 450, 450)), # U+25CC
}
# full           : 上述 cmap 全量
# jamoonly       : 删除 0xAC00..0xD7A3 内所有预组音节，保留 jamo/点/虚圆
# hole           : jamoonly 再删 U+11AB
# syllableonly   : 只留 U+D55C + 点 + U+25CC
```

## 附录 B：主要代码索引

| 主题 | 位置 |
|---|---|
| 声调码位判定 `isHangulTone` | `src/hb-ot-shaper-hangul.cc:130` |
| 零宽判定 `is_zero_width_char` | `src/hb-ot-shaper-hangul.cc:135-141` |
| 预处理/合成/分解/声调重排 | `src/hb-ot-shaper-hangul.cc:143-413` |
| L,V,T 合成整字 | `src/hb-ot-shaper-hangul.cc:283-293` |
| LV,T 合成与整字分解 | `src/hb-ot-shaper-hangul.cc:329-389` |
| `ljmo/vjmo/tjmo` 特性 | `src/hb-ot-shaper-hangul.cc:49-64` |
| shaper 描述（无 normalization、fallback pos） | `src/hb-ot-shaper-hangul.cc:441-457` |
| 字素 cluster 预合并/置标志 | `src/hb-ot-shape.cc:580-587,1183` |
| flag 最终传播 | `src/hb-ot-shape.cc:1110-1167,1198` |
| cluster 改写即清 flags | `src/hb-buffer.hh:654-660` |
| `unsafe_to_break*` 语义 | `src/hb-buffer.hh:509-552` |
| fallback 记号居中公式 | `src/hb-ot-shape-fallback.cc:246-255` |
| base 宽度取 advance、笔尖修正、清零 | `src/hb-ot-shape-fallback.cc:334,349-360,407-412` |
| glyph flag API 文档 | `src/hb-buffer.h:76-165` |
| cluster level 枚举文档 | `src/hb-buffer.h:417-495` |
| 声调回归用例 | `test/shape/data/in-house/tests/hangul-jamo.tests:4-14` |
