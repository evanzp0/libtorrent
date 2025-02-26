# ip_filter 的分析

假设：插入新规则前，m_access_list 的内容为 [(0.0.0.0, 0), (192.168.1.100, 1)],
执行 `add_rule(192.168.1.80, 192.168.1.120, 1)`后，结果为：

```
|====================================|--------------| [(0.0.0.0, 0), (192.168.1.100, 1)]
inert                      !-----------------!        [(192.168.1.80), (192.168.1.120), 1]
result                                                [(0.0.0.0, 0), (192.168.1.80, 1)]
```

分析如下：

## **初始状态**
```
[ (0.0.0.0, 0), (192.168.1.100, 1) ]
```
- 表示：
  - 从 `0.0.0.0` 到 `192.168.1.99`：权限为 `0`（禁止访问）。
  - 从 `192.168.1.100` 到 `255.255.255.255`：权限为 `1`（允许访问）。

---

## **调用 `add_rule(192.168.1.80, 192.168.1.120, 1)`**
- 起始地址：`192.168.1.80`
- 结束地址：`192.168.1.120`
- 访问权限：`1`（允许访问）

### **执行过程：**

#### **步骤 1：查找插入位置**
- 使用 `upper_bound` 查找 `192.168.1.80` 和 `192.168.1.120` 的插入位置。
- 当前 `m_access_list` 为 `[ (0.0.0.0, 0), (192.168.1.100, 1) ]`。
  - `upper_bound(192.168.1.80)` 返回指向 `(192.168.1.100, 1)` 的迭代器。
  - `upper_bound(192.168.1.120)` 返回指向 `end()` 的迭代器。
- 通过 `--i` 调整迭代器，使其指向 `(0.0.0.0, 0)`。

#### **步骤 2：处理起始地址**
- 当前规则 `(0.0.0.0, 0)` 的起始地址是 `0.0.0.0`，而新规则的起始地址是 `192.168.1.80`。
- 检查以下条件：
  ```cpp
  if (i->start != first && first_access != flags)
  ```
  - `i->start` 是 `0.0.0.0`，`first` 是 `192.168.1.80`，所以 `i->start != first` 为 `true`。
  - `first_access` 是 `0`，`flags` 是 `1`，所以 `first_access != flags` 为 `true`。
- 由于两个条件都成立，代码会进入这个分支：
  ```cpp
  i = m_access_list.insert(i, range(first, flags));
  ```
  - 在 `i` 的位置插入新规则 `(192.168.1.80, 1)`。
  - 插入后，`m_access_list` 变为：
    ```
    [ (0.0.0.0, 0), (192.168.1.80, 1), (192.168.1.100, 1) ]
    ```

#### **步骤 3：检查前一个规则的权限**
- 检查以下条件：
  ```cpp
  else if (i != m_access_list.begin() && std::prev(i)->access == flags)
  ```
  - `i` 指向 `(192.168.1.80, 1)`，是 `m_access_list` 的第二个元素，所以 `i != m_access_list.begin()` 为 `true`。
  - `std::prev(i)->access` 是 `0`，`flags` 是 `1`，所以 `std::prev(i)->access == flags` 为 `false`。
- 由于 `std::prev(i)->access == flags` 为 `false`，**条件 2 不成立**，不会进入这个分支。

#### **步骤 4：删除重叠的规则**
- 检查以下条件：
  ```cpp
  if (i != j) m_access_list.erase(std::next(i), j);
  ```
  - `i` 指向 `(192.168.1.80, 1)`。
  - `j` 指向 `end()`。
  - `i != j` 为 `true`，因为 `i` 指向有效元素，而 `j` 指向 `end()`。
  - `std::next(i)` 指向 `(192.168.1.100, 1)`。
  - 删除 `std::next(i)` 到 `j` 之间的规则（这里只有 `(192.168.1.100, 1)`）。
- **删除后**，`m_access_list` 变为：
  ```
  [ (0.0.0.0, 0), (192.168.1.80, 1) ]
  ```

#### **步骤 5：检查 `if (i->start == first)` 分支**
- 检查以下条件：
  ```cpp
  if (i->start == first)
  ```
  - `i->start` 是 `192.168.1.80`，`first` 是 `192.168.1.80`，所以 `i->start == first` 为 `true`。
- 由于条件成立，代码会进入这个分支：
  ```cpp
  const_cast<Addr&>(i->start) = first;
  const_cast<std::uint32_t&>(i->access) = flags;
  ```
  - 更新 `i->start` 为 `first`（`192.168.1.80`）。
  - 更新 `i->access` 为 `flags`（`1`）。
  - 由于 `i->start` 和 `i->access` 已经是正确的值，实际没有变化。

#### **步骤 6：处理结束地址**
- 检查是否需要插入新规则：
  ```cpp
  if ((j != m_access_list.end() && minus_one(j->start) != last)
      || (j == m_access_list.end() && last != max_addr<Addr>()))
  ```
  - `j` 指向 `end()`，且 `last` 是 `192.168.1.120`，不等于 `max_addr<Addr>()`（`255.255.255.255`）。
  - 条件成立，代码会插入新规则 `(192.168.1.121, 0)`。
  - 插入后，`m_access_list` 变为：
    ```
    [ (0.0.0.0, 0), (192.168.1.80, 1), (192.168.1.121, 0) ]
    ```

#### **步骤 7：合并规则**
- 检查是否需要合并规则：
  ```cpp
  if (j != m_access_list.end() && j->access == flags)
      m_access_list.erase(j);
  ```
  - `j` 指向 `(192.168.1.121, 0)`，访问权限是 `0`，与新规则的权限 `1` 不同。
  - 由于 `j->access != flags`，不会删除 `j` 指向的规则。

---

## **最终结果**
经过 `add_rule(192.168.1.80, 192.168.1.120, 1)` 后，`m_access_list` 的内容为：
```
[ (0.0.0.0, 0), (192.168.1.80, 1), (192.168.1.121, 0) ]
```
表示：
- 从 `0.0.0.0` 到 `192.168.1.79`：权限为 `0`（禁止访问）。
- 从 `192.168.1.80` 到 `192.168.1.120`：权限为 `1`（允许访问）。
- 从 `192.168.1.121` 到 `255.255.255.255`：权限为 `0`（禁止访问）。

---

## **经过的所有分支**
在 `add_rule` 的执行过程中，代码经过了以下分支：

### **1. 条件 1**：
```cpp
if (i->start != first && first_access != flags)
```
- **进入条件**：
  - `i->start != first` 为 `true`（`0.0.0.0 != 192.168.1.80`）。
  - `first_access != flags` 为 `true`（`0 != 1`）。
- **行为**：
  - 插入新规则 `(192.168.1.80, 1)`。

### **2. 条件 2**：
```cpp
else if (i != m_access_list.begin() && std::prev(i)->access == flags)
```
- **进入条件**：
  - `i != m_access_list.begin()` 为 `true`（`i` 指向第二个元素）。
  - `std::prev(i)->access == flags` 为 `false`（`0 != 1`）。
- **行为**：
  - 未进入该分支。

### **3. 删除重叠的规则**：
```cpp
if (i != j) m_access_list.erase(std::next(i), j);
```
- **进入条件**：
  - `i != j` 为 `true`（`i` 指向有效元素，`j` 指向 `end()`）。
- **行为**：
  - 删除 `(192.168.1.100, 1)`。

### **4. 检查 `if (i->start == first)` 分支**：
```cpp
if (i->start == first)
```
- **进入条件**：
  - `i->start == first` 为 `true`（`192.168.1.80 == 192.168.1.80`）。
- **行为**：
  - 更新 `i->start` 和 `i->access`，但由于值已经正确，实际没有变化。

### **5. 插入结束地址规则**：
```cpp
if ((j != m_access_list.end() && minus_one(j->start) != last)
    || (j == m_access_list.end() && last != max_addr<Addr>()))
```
- **进入条件**：
  - `j == m_access_list.end()` 为 `true`。
  - `last != max_addr<Addr>()` 为 `true`（`192.168.1.120 != 255.255.255.255`）。
- **行为**：
  - 插入新规则 `(192.168.1.121, 0)`。

### **6. 合并规则**：
```cpp
if (j != m_access_list.end() && j->access == flags)
    m_access_list.erase(j);
```
- **进入条件**：
  - `j != m_access_list.end()` 为 `true`。
  - `j->access == flags` 为 `false`（`0 != 1`）。
- **行为**：
  - 未进入该分支。

---

## **总结**
通过这个例子，我们覆盖了 `add_rule` 中的所有分支：
1. **插入新规则**：`if (i->start != first && first_access != flags)`
2. **合并规则**：`else if (i != m_access_list.begin() && std::prev(i)->access == flags)`
3. **删除重叠的规则**：`if (i != j) m_access_list.erase(std::next(i), j);`
4. **更新现有规则**：`if (i->start == first)`
5. **插入结束地址规则**：`if ((j != m_access_list.end() && minus_one(j->start) != last) || (j == m_access_list.end() && last != max_addr<Addr>()))`
6. **合并规则**：`if (j != m_access_list.end() && j->access == flags)`