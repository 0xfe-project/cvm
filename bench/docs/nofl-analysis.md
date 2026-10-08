# Nofl vs 我们的 CVM 实现对比

## Nofl 的核心思想（从摘要提取）

**问题：** Immix 以 line (128B) 为粒度回收，最坏情况下：
- 一个小对象（如24B）可能横跨两条 line 的边界
- 这两条 line 都被标记为"部分使用"，无法回收
- 浪费：256B - 24B = 232B（10倍浪费）

**Nofl 方案：**
- 精确到对象间的所有空隙都能回收
- 精度 = 分配对齐（如16B）
- 取名"No Free Lists"暗示了实现方式（可能直接在对象间维护空隙元数据）

**评估结果：**
- 在紧张堆大小下（tight heap）胜过 copying/mark-sweep
- 对 cache locality 有优势（连续分配）

---

## 我们的 CVM 实现现状

### 当前的 line 粒度问题

**结构：**
```c
#define CVM_LINE_SIZE 256  // 我们的 line = 256B（commix 是128B）
```

**回收粒度：**
- sweep 时按 line 标记 free/occupied
- 部分空闲的 line → free-line list（链表头在 BlockMeta）
- 小对象（24B）横跨 line 边界 → 两条 line 都"部分占用"

**实际浪费测算（tree-keep 场景）：**
- 对象：24B (header + 2指针)
- 每个 block (32KB) = 128 lines
- 如果对象随机分布，平均每个对象"钉住" 1.x 条 line
- 实际 live 524K 对象 × 24B = 12.6MB
- 报告的 heap 48MB（fixed模式），利用率 26%

**这正是 Nofl 要解决的！**

---

## 对我们实现的启示

### 优化方向1：减小 line 粒度

**当前：** 256B/line
**Commix：** 128B/line（论文原始值）
**Nofl：** 精确到对象边界（~16B 对齐）

**改动：**
```c
#define CVM_LINE_SIZE 128  // 或 64
```

**影响：**
- ✅ 减少浪费（256B → 128B，最坏浪费减半）
- ❌ line metadata 翻倍（每个 block 的 line 数量翻倍）
- ❌ sweep 循环次数翻倍

**收益预估：**
- tree-keep 堆从 48MB → ~35MB（利用率 26% → 36%）
- read-heavy 类似改善

---

### 优化方向2：精确空隙回收（Nofl 方案）

**核心思路（猜测，论文未完整下载）：**
- 不维护 free-line list，而是在对象间直接记录"下一个空隙"
- sweep 时：逐对象扫描，相邻对象间的空隙形成"hole"链表
- 分配时：从 hole 链表取空隙，如果不够大则合并或跳过

**伪代码（猜测）：**
```c
// 每个空隙的头部记录
struct Hole {
    u32 size;      // 这个空隙的大小
    Hole* next;    // 下一个空隙
};

// sweep 时
for (obj in block) {
    if (!marked(obj)) {
        // 这个对象是garbage，变成hole
        Hole* h = (Hole*)obj;
        h->size = obj->size;
        h->next = current_hole_list;
        current_hole_list = h;
    }
}

// 分配时
for (h in hole_list) {
    if (h->size >= requested) {
        return h;  // 精确分配
    }
}
```

**挑战：**
- 需要改动整个分配路径（不再是 bump pointer 为主）
- hole 链表的遍历成本（可能比 line-granular 的 bitmap 慢）
- 但在"紧张堆"场景下（内存受限），精确回收的优势明显

---

### 优化方向3：混合策略

**思路：**
- 保留 bump pointer 快速路径
- 只在"堆紧张"时启用精确回收
- 类似 Immix 的 defragmentation 触发条件

**实现：**
```c
if (heap_utilization < 0.5) {
    // 堆宽松，用 line-granular（快）
    allocate_from_free_lines();
} else {
    // 堆紧张，用 precise holes（省空间）
    allocate_from_holes();
}
```

---

## 对当前性能问题的解释

### read-heavy pause 慢（3.5ms vs bdwgc 2.4ms）

**可能原因（结合 Nofl）：**
- 我们的 line metadata 开销：每个 block 128条line × 元数据
- 标记时不仅要标对象，还要更新 line 的 occupied 状态
- bdwgc 的 mark-sweep 没有 line 粒度，只标对象 → 更简单

**验证：**
- 查看标记路径是否每个对象都更新 line metadata
- 如果是，这就是额外开销

### tree-keep 内存利用率低（26%）

**就是 line 粒度浪费：**
- 24B 对象 × 524K = 12.6MB live
- 48MB heap → 利用率 26%
- 如果用 Nofl 精确回收：理论上 12.6MB × 2 = ~25MB 够用

---

## 立即可做的优化

**1. 减小 CVM_LINE_SIZE 到 128B（匹配 commix）**
- 改动量：1行常量
- 风险：低（只是参数调整）
- 预期收益：内存利用率提升 ~30%

**2. 检查标记路径是否有不必要的 line metadata 更新**
- 如果每次标记对象都写 line 的状态 → 改成只在 sweep 时更新
- 预期收益：read-heavy pause 减少 10–20%

**3. 长期：实现 Nofl 式的精确空隙回收**
- 需要重写分配器核心
- 但在"紧张堆"场景下能接近理论最优

---

## 结论

**Nofl 的启示：我们的 line-granular 回收在内存利用率上有改进空间。**
- 短期：减小 line size（128B）
- 中期：优化标记路径，减少 line metadata 写入
- 长期：考虑实现精确空隙回收（Nofl-like）

当前性能已经接近预期，但在"紧张堆"场景下（如嵌入式、容器限制），Nofl 的思路值得借鉴。
