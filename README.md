# Win32 RTOS風ディスパッチひな形（C言語, Interlocked版）

## 概要
Win32スレッドと同期オブジェクトを利用して、RTOS風のタスクディスパッチ機能を実装した最小構成のひな形です。  
C11 `_Atomic` 非対応環境でも動作するように、状態管理は `volatile` + Win32 `Interlocked*` API で実装しています。

## 主な特徴
- **タスク管理**  
  - 生成、開始、停止、削除
  - ユーザー定義関数をタスクとして実行可能
  - 協調的停止（タスク側で `rtos_should_stop()` を確認）
  - 実行権譲渡（`rtos_yield()`）
- **同期**  
  - イベント（manual-reset event）によるタスク間同期
  - タイムアウト付き待機 / 無限待機
- **時間管理**  
  - 10ms単位のシステムティック（Waitable Timer使用）
  - 遅延 (`rtos_delay_ticks`)  
- **依存ライブラリなし**  
  - Win32 APIのみ使用

---

## API一覧

### ランタイム制御
| 関数 | 概要 |
|------|------|
| `bool rtos_start(void)` | システムタイマーとtickスレッドを開始 |
| `void rtos_shutdown(void)` | タイマー停止・tickスレッド終了 |

### 時間
| 関数 | 概要 |
|------|------|
| `uint64_t rtos_get_ticks(void)` | 現在のtick値（10ms単位）を取得 |
| `void rtos_delay_ticks(uint32_t ticks)` | ticks × 10ms 遅延（Sleep使用） |

### タスク管理
| 関数 | 概要 |
|------|------|
| `bool rtos_task_create(rtos_task_t **out, const char* name, rtos_task_entry_t entry, void* arg, int stack_hint, int prio_hint)` | タスク生成（開始しない） |
| `bool rtos_task_start(rtos_task_t *t)` | タスク開始（サスペンド解除） |
| `void rtos_task_stop(rtos_task_t *t)` | 停止要求フラグを立てる（協調停止） |
| `void rtos_task_delete(rtos_task_t *t, uint32_t join_timeout_ticks)` | 終了待ち＋リソース解放 |
| `bool rtos_should_stop(void)` | タスク内から停止要求フラグを確認 |
| `void rtos_yield(void)` | 実行権を明示的に譲る（協調実行向け） |

### イベント（同期）
| 関数 | 概要 |
|------|------|
| `bool rtos_event_create(rtos_event_t* ev)` | イベント生成（manual-reset） |
| `void rtos_event_destroy(rtos_event_t* ev)` | イベント破棄 |
| `void rtos_event_reset(rtos_event_t* ev)` | 非シグナル状態へリセット |
| `void rtos_event_set(rtos_event_t* ev)` | シグナル状態に設定 |
| `bool rtos_wait_event(rtos_event_t* ev, uint32_t timeout_ticks)` | イベント待ち（ticks単位のタイムアウト可） |

---

## 実行の流れ（例）
1. `rtos_start()` でtickスレッド開始
2. 必要なイベントを `rtos_event_create()` で作成
3. `rtos_task_create()` でタスク生成
4. `rtos_task_start()` でタスク開始
5. タスク内で `rtos_should_stop()` を監視しながら処理
6. 必要に応じて `rtos_yield()` で協調実行
7. 終了時に `rtos_task_stop()` → `rtos_task_delete()` でリソース解放
8. 最後に `rtos_shutdown()` でシステム終了

---

## デモタスク例
```c
static void user_task_A(void* arg) {
    (void)arg;
    printf("[A] start\n");
    while (!rtos_should_stop()) {
        printf("[A] tick=%llu\n", (unsigned long long)rtos_get_ticks());
        rtos_delay_ticks(50); // 500ms
        rtos_yield();
    }
    printf("[A] stop\n");
}

static void user_task_B(void* arg) {
    (void)arg;
    printf("[B] start (wait event)\n");
    while (!rtos_should_stop()) {
        bool ok = rtos_wait_event(&g_evt, 300); // 3秒
        if (ok) {
            printf("[B] event signaled!\n");
            rtos_event_reset(&g_evt);
        } else {
            printf("[B] timeout\n");
        }
        rtos_yield();
    }
    printf("[B] stop\n");
}
````

---

## ビルド例

### MSVC

```powershell
cl /TC /W4 /O2 /MT /std:c11 rtos_win32_interlocked.c
```

### MinGW

```bash
gcc -O2 -Wall -Wextra -o rtos_win32_interlocked.exe rtos_win32_interlocked.c
```

---

## 制限と拡張

* **完全協調スケジューリング**のため、タスク側が`rtos_yield()`や待機APIを呼ばないとCPUを譲らない
* 優先度制御は未実装（`SetThreadPriority`で拡張可能）
* 同期はイベントのみ（セマフォやキューなどは追加可能）
* 時間精度は10ms単位（`timeBeginPeriod`などで改善可能）

---
