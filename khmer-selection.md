# 高棉文整形输出与原文选区的映射分析

供实现文字命中测试（hit-testing）/ 选区映射的同事使用。
结论先行：**不要按"屏幕上第几个字形"反推原文偏移**。高棉文整形会把字
形在视觉轴上重排（分裂元音左半移到基字之前、COENG+RO 移到音节最前），
并把多个原文字符合并进一个字形（下标辅音连字、基字+元音连字）。唯一可
靠的纽带是 `hb_glyph_info_t.cluster`，而 cluster 的粒度由
`hb_buffer_cluster_level_t` 和整形过程中的合并行为共同决定。本文给出代
码依据、一个实测词段的完整映射表，以及"哪些归属可恢复、哪些不可恢复"
的判定结论。

## 1. 整形流水线中和选区有关的环节

代码位置：`src/hb-ot-shaper-khmer.cc`、`src/hb-ot-shaper-khmer-machine.rl`、
`src/hb-buffer.cc` / `src/hb-buffer.hh`、`src/hb-ot-layout-gsubgpos.hh`。

### 1.1 规范化：分裂元音一拆二

`decompose_khmer()`（`src/hb-ot-shaper-khmer.cc:328`）把没有 Unicode 分解
的五个分裂元音拆开：

| 原字符 | 分解为 | 说明 |
|---|---|---|
| U+17BE | U+17C1 + U+17BE | |
| U+17BF | U+17C1 + U+17BF | |
| U+17C0 | U+17C1 + U+17C0 | |
| U+17C4 | U+17C1 + U+17C4 | |
| U+17C5 | U+17C1 + U+17C5 | |

U+17C1 是**画在基字左边**的元音（类别 VPre），第二段留在原位置（VPst）。
规范化阶段拆出的两个字符**都继承原字符的 cluster 值**
（`src/hb-ot-shape-normalize.cc`，拆分时不改 cluster）。`compose_khmer()`
（同文件 `:351`）拒绝把拆分元音再合回去，所以这两半会以两个独立字符进
入后续流程。

**对选区的含义**：一个原文字符（如 U+17C4）对应两个最终会出现在屏幕不
同位置的字形——左半在基字之前，右半在基字之后（或并入基字连字）。

### 1.2 音节切分与 unsafe_to_break

`setup_syllables_khmer()`（`src/hb-ot-shaper-khmer.cc:189`）用 Ragel 状态
机 `find_syllables_khmer()`（`src/hb-ot-shaper-khmer-machine.rl:113`）把
buffer 切成音节，然后对**每个音节的完整范围**调用
`buffer->unsafe_to_break(start, end)`（`:196-201`）。语法要点
（`.rl:79-93`）：`consonant_syllable = (cn|PLACEHOLDER|DOTTEDCIRCLE)
broken_cluster`，其中 `broken_cluster` 允许 `(H.cn)*`——即任意多组
COENG(U+17D2，类别 H)+辅音 的下标序列都归进**同一个音节**。

**对选区的含义**：音节内部任意两点之间都被标记为不可断开
（`HB_GLYPH_FLAG_UNSAFE_TO_BREAK`）。如果调用方对"选中片段"单独重新整
形（例如局部重排、复制后重排），不扩展到音节边界就会得到不同的字形。

### 1.3 重排：两处会搬动字形的位置

`reorder_consonant_syllable()`（`src/hb-ot-shaper-khmer.cc:208`）在每个音
节内做两类搬动，**每次搬动前都先 `merge_clusters`**：

1. **COENG+RO 前移**（`:242-270`）：遇到 `H`（U+17D2）后跟 `Ra`
   （U+179A）时，先 `buffer->merge_clusters(start, i+2)` 把音节开头到
   RO 为止的所有字符并成一个 cluster，再把 COENG,RO 两个字符 memmove
   到音节最前，并打上 `pref` feature 掩码。后续字符打 `cfar` 掩码。
2. **VPre 前移**（`:274-281`）：遇到左画元音（U+17C1 等）时，先
   `buffer->merge_clusters(start, i+1)`，再把它 memmove 到音节开头
   （基字之前）。

`merge_clusters_impl()`（`src/hb-buffer.cc:562`）的语义：把 `[start,end)`
内所有字符的 cluster 统一为其中的**最小值**，并向两侧扩展已同值的相邻
run。也就是说，被搬动的字符从此"失去"自己原来的偏移，对外表现为属于
基字所在的 cluster。这是单调 cluster level（0/1）下保持输出 cluster 非
递减的手段；在非单调 level（2/3）下 `merge_clusters` 退化为只打
`unsafe_to_break` 标记（`src/hb-buffer.hh:396-405`）。

### 1.4 GSUB 替换：下标辅音连字与基字连字

feature 分两组应用（`src/hb-ot-shaper-khmer.cc:41-62`）：`pref/blwf/abvf/
pstf/cfar` 按音节逐个应用，`pres/abvs/blws/psts` 全局应用。字体在这些
feature 里做两类和选区强相关的替换：

- **COENG+辅音 → 单个下标字形**（如 `uni17D2179A`）：零 advance、带负
  的 x/y offset，画在基字（或音节首）下方。
- **基字+元音右半 → 连字**（如 `uni178017B6`）：一个原文字符的右半并进
  基字字形。

连字形成时 `ligate_input()` 调 `buffer->merge_clusters(buffer->idx,
match_end)`（`src/hb-ot-layout-gsubgpos.hh:2210`）：单调 level 下连字成
员的 cluster 全部并成最小值；连字字形本身携带第一个成员的 cluster。

### 1.5 cluster level 的语义

`hb_buffer_cluster_level_t`（`src/hb-buffer.h:414-460`）：

- **0 = MONOTONE_GRAPHEMES（默认）**：非基字符并入前面基字的 cluster；
  任何会破坏单调性的操作都触发合并。粒度最粗。
- **1 = MONOTONE_CHARACTERS**：非基字符先保留自己的 cluster，只在重排
  /连字等必要时合并。输出仍单调。**官方推荐新客户端使用**。
- **2 = CHARACTERS**：不合并、不保证单调。信息最全，但 cluster 序列会乱
  序（高棉重排后必然乱序），客户端不能二分。
- 3 = GRAPHEMES：按字素合并但不强制单调。

## 2. 实测词段：最终绘制次序与原文范围映射

字体：`test/shape/data/in-house/fonts/3998336402905b8be8301ef7f47cf7e050cbb1bd.ttf`
（仓库 shape 测试字体，用例见
`test/shape/data/in-house/tests/khmer-misc.tests`）。
工具：`build/util/hb-shape`，输出顺序即 LTR 绘制次序。

### 2.1 主例：分裂元音 + COENG+RO 同音节

词段：`U+1780 U+17C4 U+17D2 U+179A`（KA + 分裂元音 OO + COENG + RO，
即 khmer-misc.tests 第 21 条）。

整形内部过程：

1. 规范化：U+17C4 → U+17C1（VPre，画左边）+ U+17C4（VPst，右半），两
   者 cluster 都是 1。
2. 重排：COENG+RO 命中 `pref` 规则，`merge_clusters(0,4)` 后移到音节
   最前；VPre 的合并范围已被前者覆盖。
3. GSUB：`pref` 把 COENG+RO 换成单个字形 `uni17D2179A`；基字 KA 与元
   音右半连字成 `uni178017B6`。

最终绘制次序（`--cluster-level=0`，`=1` 输出相同）：

| 绘制序 | 字形 | cluster | advance | x,y offset | 来源字符（逻辑序） |
|---|---|---|---|---|---|
| 0 | `uni17D2179A` | 0 | +287 | 0,0 | U+17D2+U+179A（原文第 3、4 字！） |
| 1 | `uni17C1` | 0 | +288 | 0,0 | U+17C4 的左半（原文第 2 字） |
| 2 | `uni178017B6` | 0 | +924 | 0,0 | U+1780 + U+17C4 的右半（原文第 1、2 字） |

屏幕上从左到右是"下标 RO 形 → 元音左半 → 基字+元音右半"，与原文顺序
完全交错。三个字形 cluster 全为 0，即**整个音节是一个不可分的选择单
元**：

| 想选的原文范围 | 实际命中（cluster 映射） | 屏幕高亮 |
|---|---|---|
| [0,4) 整个音节 | cluster 0 → 全部 3 字形 | 整个音节 |
| 仅 U+1780（char 0） | cluster 0 → 全部 3 字形 | 整个音节 |
| 仅 U+17C4（char 1） | cluster 0 → 全部 3 字形 | 整个音节 |
| 仅 U+17D2,U+179A（char 2-3） | cluster 0 → 全部 3 字形 | 整个音节 |

对照 `--cluster-level=2`（不合并，信息上限）：

```
[uni17D2179A=2+287|uni17C1=1+288|uni178017B6=0+924]
```

cluster 序列 2,1,0 —— 非单调，直接证明发生了重排。此时：

| 原文范围 | 对应字形 | 备注 |
|---|---|---|
| char 0（KA） | `uni178017B6` | 与 char 1 共享，无法独占 |
| char 1（OO） | `uni17C1` + `uni178017B6` | 一个字符 → 屏幕上两处、中间隔着别的内容 |
| char 2-3（COENG+RO） | `uni17D2179A` | 两字符合一字形，内部边界不可见 |

即使在 level 2，char 0 与 char 1 仍共享连字 `uni178017B6`——**连字内部
的字符边界从输出中不可恢复**（见 §3）。

### 2.2 辅例：下标辅音不重排时，归属可以保住

词段：`U+1798 U+17D2 U+1796 U+17BB`（MO + COENG + KHO + 下元音 U，
khmer-misc.tests 第 5 条）。COENG 后不是 RO，不触发前移。

`--cluster-level=1`：

```
[uni1798=0+635|uni17D21796=1@-1,-26+0|uni17BB=3@-22,-296+0]
```

| 原文范围 | 字形 | 说明 |
|---|---|---|
| char 0 | `uni1798`（cluster 0） | 基字 |
| char 1-2（COENG+KHO） | `uni17D21796`（cluster 1） | 下标连字，advance=0，offset (-1,-26)，画在基字下方 |
| char 3 | `uni17BB`（cluster 3） | 下元音，零宽负偏移 |

这里每个可选范围与字形一一对应：下标连字的成员在原文中**连续**、没有
被搬动，连字 cluster 取成员最小值（1），边界恰好落在字符边界上。但注
意 `--cluster-level=0`（默认）下同一词段输出全部 cluster=0——字素级合
并把整串并掉，子范围选择依旧连带整个音节。

### 2.3 双音节词：音节边界是 cluster 边界

词段：`U+1792 U+17D2 U+179B U+17C4 U+1780 U+17CB`（两个音节，
khmer-misc.tests 第 56 条），level 0：

```
[uni17C1=0+288|uni179217B6=0+923|uni17D2179B=0@-290,-26+0|uni1780=4+636|uni17CB=4@-23,-29+0]
```

音节一（char 0-3）全部 cluster 0：绘制序是"元音左半 → 基字+右半连字 →
下标连字（offset -290 拉回基字下方）"。音节二（char 4-5）cluster 4。
**选区可以安全地以音节为粒度切分**；音节之间 cluster 单调递增，可二分。

## 3. 下标辅音参与替换时：可确定与不可恢复的归属

**能由整形 cluster 确定的：**

- 字形 → 原文范围的下界：cluster 值即该字形所属逻辑段的第一个字符偏
  移；配合下一个不同 cluster 值得到范围上界（单调 level 下）。
- 下标连字（COENG+X → 单字形）的整体归属：连字成员在原文中连续时
  （未触发 COENG+RO 前移），level ≥1 下连字 cluster = 成员最小偏移，
  成员范围 = [cluster, 下一 cluster)，精确可还原（§2.2）。
- 音节边界：cluster 值在音节间严格递增；`unsafe_to_break` 标记给出同
  一结论。

**无法从输出独立恢复的：**

- **连字内部各字符的边界与宽度**。`uni17D2179A` 里 COENG 与 RO 各占多
  少、`uni178017B6` 里 KA 与元音右半各占多少，cluster 不记录（合并只
  保留最小值，`hb-buffer.cc:562`），advance 也不按字符拆分。需要连字
  内部光标位置时只能用 `hb_ot_layout_get_ligature_carets()` 查字体的
  插入符表，且字体未必提供。
- **被重排字符的原始偏移**（level 0/1）。COENG+RO 前移和 VPre 前移前
  的 `merge_clusters` 把被搬动字符的 cluster 改写为基字的值；从输出无
  法知道 `uni17C1` 原本在 char 1 还是别处。只有 level 2 保留（代价是
  非单调）。
- **分裂元音两半的同源性**。仅从输出看，`uni17C1` 与 `uni178017B6` 是
  两个无关字形；只有结合 §1.1 的分解表（或 level 2 下两者 cluster 都
  等于原元音偏移……注意右半并入基字连字后连这一点也丢了）才能知道它
  们来自同一个 U+17C4。
- **level 0（默认）下音节内的一切细分**。字素级合并 + 重排合并叠加，
  高棉音节通常塌缩成单个 cluster（§2.1、§2.3）。

## 4. 对命中测试 / 选区实现的结论

1. **映射一律走 cluster，不走视觉序号**。x 坐标 → 命中字形 →
   `glyph.cluster` → 原文范围 [cluster, 下一 cluster)；原文范围 → 所有
   cluster 落在该范围内的字形。同一 cluster 的多个字形必须整体选中、
   整体高亮。
2. **一次局部选择连带同一音节其他部分的情形**（默认 level 0/1 下必然
   触发，level 2 下部分触发）：
   - 音节含分裂元音（U+17BE–U+17C0、U+17C4、U+17C5）：左半被前移并
     合并，选元音 = 选基字 = 选整个前段；
   - 音节含 COENG+RO：`merge_clusters(start, i+2)` 把音节开头到 RO 全
     部并掉，选其中任意字符连带整个前段；
   - COENG+X 形成下标连字：连字成员不可分，选 COENG 或 X 都选中整个
     下标字形；
   - 基字与元音/符号形成连字（如 `uni178017B6`）：同样不可分；
   - level 0 下以上全部叠加字素级合并，实务上**高棉音节就是最小选择
     单元**。
3. **需要更细粒度时**：用 `HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS`
   （1）。无重排的音节可精确到 COENG 对（§2.2）；但 §1.3 的两类前移
   仍会强制合并其覆盖范围，不要假设 level 1 能选到元音或 RO 本身。
   level 2 信息最全但 cluster 非单调，映射逻辑必须容忍乱序，且连字内
   部边界依然不可恢复。
4. **局部重整形必须扩展到音节边界**：整个音节被标
   `HB_GLYPH_FLAG_UNSAFE_TO_BREAK`（`hb-ot-shaper-khmer.cc:196-201`），
   在音节内部切开重排会得到不同字形（下标形、左元音位置都依赖完整
   音节上下文）。
5. **破碎输入**：以 COENG 或元音开头的 broken cluster 会被插入
   U+25CC 虚圈（`hb_syllabic_insert_dotted_circles`，
   `src/hb-ot-shaper-syllabic.cc:33`），虚圈字形取后随字符的 cluster，
   选区映射时把它归入该 cluster 即可，不要为它发明原文偏移。
