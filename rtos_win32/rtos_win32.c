// rtos_win32_interlocked.c  (Win32, C, _Atomic不使用版)
// Build (MSVC): cl /TC /W4 /O2 /MT /std:c11 rtos_win32_interlocked.c
// Build (MinGW): gcc -O2 -Wall -Wextra -o rtos_win32_interlocked.exe rtos_win32_interlocked.c
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_MSC_VER)
#define TLS __declspec(thread)
#else
#define TLS __thread
#endif

// ====== 時間基準（10ms tick） =================================================

static HANDLE g_tickTimer = NULL;    // waitable timer (auto-reset)
static HANDLE g_tickThread = NULL;   // tick thread
static volatile LONG64 g_tick = 0;   // 10ms単位のシステムティック

// 10msごとに1 tick進める
static DWORD WINAPI tick_thread_proc(LPVOID arg) {
	(void)arg;
	LARGE_INTEGER due;
	due.QuadPart = -1; // ほぼ即時 (relative)
	HANDLE timer = CreateWaitableTimer(NULL, FALSE, NULL); // auto-reset=false? 第2引数FALSEでauto-reset
	if (!timer) return 1;

	if (!SetWaitableTimer(timer, &due, 10, NULL, NULL, FALSE)) {
		CloseHandle(timer);
		return 2;
	}
	g_tickTimer = timer;

	for (;;) {
		DWORD w = WaitForSingleObject(timer, INFINITE);
		if (w != WAIT_OBJECT_0) break;
		InterlockedAdd64(&g_tick, 1);
	}
	CloseHandle(timer);
	g_tickTimer = NULL;
	return 0;
}

uint64_t rtos_get_ticks(void) {
	return (uint64_t)InterlockedCompareExchange64(&g_tick, 0, 0);
}

// 10ms単位の遅延（簡易：Sleep使用）
void rtos_delay_ticks(uint32_t ticks) {
	if (ticks == UINT32_MAX) {
		Sleep(INFINITE);
	}
	else {
		DWORD ms = ticks * 10u;
		Sleep(ms);
	}
}

// ====== イベント（待ち/解除） =================================================

typedef struct {
	HANDLE h;
} rtos_event_t;

bool rtos_event_create(rtos_event_t* ev) {
	if (!ev) return false;
	ev->h = CreateEvent(NULL, TRUE, FALSE, NULL); // manual-reset, non-signaled
	return ev->h != NULL;
}

void rtos_event_destroy(rtos_event_t* ev) {
	if (ev && ev->h) {
		CloseHandle(ev->h);
		ev->h = NULL;
	}
}

void rtos_event_reset(rtos_event_t* ev) {
	if (ev && ev->h) ResetEvent(ev->h);
}

void rtos_event_set(rtos_event_t* ev) {
	if (ev && ev->h) SetEvent(ev->h);
}

// timeout_ticks: UINT32_MAXで無限待ち
// return: true=シグナル, false=タイムアウト
bool rtos_wait_event(rtos_event_t* ev, uint32_t timeout_ticks) {
	if (!ev || !ev->h) return false;
	DWORD to = (timeout_ticks == UINT32_MAX) ? INFINITE : (timeout_ticks * 10u);
	DWORD w = WaitForSingleObject(ev->h, to);
	return (w == WAIT_OBJECT_0);
}

// ====== タスク（スレッド） ====================================================

typedef void(*rtos_task_entry_t)(void* arg);

typedef enum {
	RTOS_TASK_NEW = 0,
	RTOS_TASK_RUNNING,
	RTOS_TASK_STOPPING,
	RTOS_TASK_STOPPED
} rtos_task_state_t;

typedef struct rtos_task {
	HANDLE thread;
	DWORD  tid;
	rtos_task_entry_t entry;
	void*  arg;

	// 協調停止フラグ・状態（Interlockedで扱えるようLONGに）
	volatile LONG stop_req; // 0=false, 1=true
	volatile LONG state;    // rtos_task_state_t をLONGで持つ

	// 協調スケジューリング用のyieldイベント
	HANDLE run_event;       // signaledで実行許可
	const char* name;
} rtos_task_t;

// スレッドローカル：現在のタスク
static TLS rtos_task_t* tls_self = NULL;

// 「yield」= 実行権を明示的に譲る（協調スケジューリング）
void rtos_yield(void) {
	rtos_task_t* t = tls_self;
	if (!t) { Sleep(0); return; } // 非タスクスレッドならOSに譲る
	ResetEvent(t->run_event);
	Sleep(0);           // 他スレッドにスライス譲渡
	SetEvent(t->run_event); // 自分を再開可能に
}

bool rtos_should_stop(void) {
	rtos_task_t* t = tls_self;
	if (!t) return false;
	return InterlockedCompareExchange(&t->stop_req, 0, 0) != 0;
}

// タスク用ラッパ
static DWORD WINAPI task_trampoline(LPVOID param) {
	rtos_task_t* t = (rtos_task_t*)param;
	tls_self = t;

	InterlockedExchange(&t->state, RTOS_TASK_RUNNING);

	// 開始APIでrun_eventがSetされるまで待機
	WaitForSingleObject(t->run_event, INFINITE);

	t->entry(t->arg);

	InterlockedExchange(&t->state, RTOS_TASK_STOPPED);
	return 0;
}

// 生成（開始はしない）
bool rtos_task_create(rtos_task_t** out,
	const char* name,
	rtos_task_entry_t entry, void* arg,
	int stack_bytes_hint, int prio_hint) {
	if (!out || !entry) return false;

	rtos_task_t* t = (rtos_task_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
	if (!t) return false;

	t->entry = entry;
	t->arg = arg;
	t->name = name;
	InterlockedExchange(&t->stop_req, 0);
	InterlockedExchange(&t->state, RTOS_TASK_NEW);

	t->run_event = CreateEvent(NULL, TRUE, FALSE, NULL); // manual-reset
	if (!t->run_event) {
		HeapFree(GetProcessHeap(), 0, t);
		return false;
	}

	HANDLE h = CreateThread(NULL, 0, task_trampoline, t, CREATE_SUSPENDED, &t->tid);
	if (!h) {
		CloseHandle(t->run_event);
		HeapFree(GetProcessHeap(), 0, t);
		return false;
	}
	t->thread = h;
	*out = t;
	return true;
}

// 開始（サスペンド解除＋run_eventをセット）
bool rtos_task_start(rtos_task_t* t) {
	if (!t) return false;
	if (InterlockedCompareExchange(&t->state, RTOS_TASK_NEW, RTOS_TASK_NEW) != RTOS_TASK_NEW)
		return false;
	SetEvent(t->run_event);
	return (ResumeThread(t->thread) != (DWORD)-1);
}

// 停止要求（協調停止）
void rtos_task_stop(rtos_task_t* t) {
	if (!t) return;
	InterlockedExchange(&t->stop_req, 1);
	SetEvent(t->run_event); // 待っていたら起こす
}

// 削除（終了待ち→クローズ）
void rtos_task_delete(rtos_task_t* t, uint32_t join_timeout_ticks) {
	if (!t) return;
	DWORD to = (join_timeout_ticks == UINT32_MAX) ? INFINITE : (join_timeout_ticks * 10u);
	WaitForSingleObject(t->thread, to);
	CloseHandle(t->thread);
	CloseHandle(t->run_event);
	HeapFree(GetProcessHeap(), 0, t);
}

// ====== ランタイム起動/終了 ===================================================

bool rtos_start(void) {
	DWORD tid;
	g_tickThread = CreateThread(NULL, 0, tick_thread_proc, NULL, 0, &tid);
	return g_tickThread != NULL;
}

void rtos_shutdown(void) {
	if (g_tickTimer) CancelWaitableTimer(g_tickTimer);
	if (g_tickThread) {
		// 早めに抜ける（SetWaitableTimerキャンセル済み）
		WaitForSingleObject(g_tickThread, 100);
		CloseHandle(g_tickThread);
		g_tickThread = NULL;
	}
}

// ====== デモタスク ============================================================

static rtos_event_t g_evt;

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
		}
		else {
			printf("[B] timeout\n");
		}
		rtos_yield();
	}
	printf("[B] stop\n");
}

// ====== エントリ（デモ） =====================================================

int main(void) {
	if (!rtos_start()) {
		fprintf(stderr, "rtos_start failed\n");
		return 1;
	}
	rtos_event_create(&g_evt);

	rtos_task_t* ta = NULL;
	rtos_task_t* tb = NULL;

	rtos_task_create(&ta, "A", user_task_A, NULL, 0, 0);
	rtos_task_create(&tb, "B", user_task_B, NULL, 0, 0);

	rtos_task_start(ta);
	rtos_task_start(tb);

	// 2秒後にイベント発火
	rtos_delay_ticks(200);
	printf("[MAIN] set event\n");
	rtos_event_set(&g_evt);

	// さらに2秒後に停止要求
	rtos_delay_ticks(200);
	printf("[MAIN] stop tasks\n");
	rtos_task_stop(ta);
	rtos_task_stop(tb);

	// 終了待ち＆削除
	rtos_task_delete(ta, 100); // 1秒待ち
	rtos_task_delete(tb, 100);

	rtos_event_destroy(&g_evt);
	rtos_shutdown();
	printf("[MAIN] done\n");
	return 0;
}
