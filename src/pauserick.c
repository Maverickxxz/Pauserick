/*
 * Pauserick - a tiny, strict break reminder for Windows.
 *
 * Plain Win32/C, no runtime, no framework. Between breaks the process only owns
 * a hidden window, a tray icon and one coalescable timer that wakes at most every
 * few seconds. During a break it shows a black full-screen window on every
 * monitor and swallows all keyboard and mouse input with low-level hooks.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stddef.h>
#include <stdarg.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>
#include <sddl.h>
#include <roapi.h>
#include <winstring.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <string.h>
#include "resource.h"
#include "version.h"

#define APP_NAME            L"Pauserick"
#define APP_VERSION         L"" PAUSERICK_VER_STR

/* Strict mode = an elevated logon task. A test build (-DPAUSERICK_TEST_TASK) uses
 * separate names and no elevation so the whole flow can be exercised without UAC. */
#ifdef PAUSERICK_TEST_TASK
#define TASK_NAME        L"PauserickTest"
#define RUN_NAME         L"PauserickTest"
#define TASK_RUNLEVEL    L"LeastPrivilege"
#define TASK_USER_RIGHTS L"FA"
#define TASK_ELEVATE     FALSE
#define MUTEX_NAME       L"Local\\PauserickTest.SingleInstance"
#define CLS_MAIN         L"PauserickTestMain"
#else
#define TASK_NAME        L"Pauserick"
#define RUN_NAME         APP_NAME
#define TASK_RUNLEVEL    L"HighestAvailable"
#define TASK_USER_RIGHTS L"GRGX"
#define TASK_ELEVATE     TRUE
#define MUTEX_NAME       L"Local\\Pauserick.SingleInstance"
#define CLS_MAIN         L"PauserickMain"
#endif
#define CLS_OVERLAY         L"PauserickOverlay"
#define CLS_WARN            L"PauserickWarn"

#define WM_APP_TRAY          (WM_APP + 1)
#define WM_APP_SHOW_SETTINGS (WM_APP + 2)
#define WM_APP_BREAK_NOW     (WM_APP + 3) /* wParam: 0 = mini break, 1 = break */
#define WM_APP_UPDATE        (WM_APP + 4) /* lParam: UpdInfo * from the update check thread */
#define WM_APP_UPDATE_PROMPT (WM_APP + 5) /* offer the new version now */
#define WM_APP_UPDATE_DONE   (WM_APP + 6) /* wParam: result of installing it */

#define TIMER_TICK          1
#define TIMER_REBUILD       2
#define TIMER_UPDATE        3

#define IDM_TAKE_MINI       2001
#define IDM_TAKE_LONG       2002
#define IDM_SETTINGS        2003
#define IDM_EXIT            2004
#define IDM_OPEN_LOG        2005
#define IDM_UPDATE          2006

#define MAX_OVERLAYS        16
#define IDLE_TICK_MS        5000u

enum { BT_MINI = 0, BT_LONG = 1 };
enum { ST_IDLE, ST_WARN, ST_BREAK };
enum { PAUSE_LOCK = 1, PAUSE_SUSPEND = 2, PAUSE_IDLE = 4 };
#define PAUSE_AWAY (PAUSE_LOCK | PAUSE_SUSPEND) /* the PC itself is locked or asleep */

/* ---------------------------------------------------------------- settings */

typedef struct {
    int miniEnabled, miniEveryMin, miniDurSec;
    int longEnabled, longEveryMin, longDurMin;
    int warnSec, sound;
    int idlePause;     /* pause when the PC is idle */
    int idlePauseMin;  /* ...for this long */
    int idleContinue;  /* when back: 1 = timers continue where they stopped, 0 = restart from zero */
    int lang;          /* LANG_EN / LANG_IT */
    int gaming;        /* no breaks while a game is in the foreground */
    int autoUpdate;    /* look for a new version once a day */
    WCHAR games[512];  /* extra games: exe names or folders, separated by ';' */
} Settings;

/* ------------------------------------------------------------ strings */

enum { LANG_EN = 0, LANG_IT = 1, LANG_COUNT };

enum {
    S_MINI_BREAK, S_BREAK, S_MINI_BREAK_LC, S_BREAK_LC,
    S_NEXT_FMT, S_IN_MIN_FMT, S_IN_SEC_FMT, S_IN_CLOCK_FMT,
    S_MINI_RUNNING, S_BREAK_RUNNING, S_PAUSED, S_PAUSED_IDLE, S_NONE,
    S_TAKE_MINI, S_TAKE_LONG, S_SETTINGS_MENU, S_EXIT,
    S_DLG_TITLE, S_ENABLE_MINI, S_ENABLE_LONG, S_EVERY, S_DURATION, S_OFF,
    S_GENERAL, S_AUTOSTART, S_STRICT, S_SOUND, S_WARN, S_IDLE,
    S_WHEN_BACK, S_RESUME_RESTART, S_RESUME_CONTINUE,
    S_MINI_DESC, S_LONG_DESC, S_IDLE_GROUP, S_IDLE_DESC, S_AFTER, S_GAMING_DESC, S_GAMES_AUTO,
    S_STRICT_HINT, S_AUTOUPDATE,
    S_UPD_CHECK, S_UPD_CHECKING, S_UPD_LATEST, S_UPD_CHECK_FAILED, S_UPD_GET_FMT, S_UPD_TRAY_FMT,
    S_UPD_TITLE_FMT, S_UPD_TEXT_FMT, S_UPD_INSTALL, S_UPD_LATER, S_UPD_SKIP, S_UPD_NOTES, S_UPD_HIDE_NOTES,
    S_UPD_FAILED_FMT, S_UPD_E_NET, S_UPD_E_SIG, S_UPD_E_FILE, S_UPD_E_RIGHTS,
    S_LANGUAGE, S_CLOSE, S_GITHUB,
    S_ERR_STRICT_ON, S_ERR_STRICT_OFF, S_ERR_AUTOSTART,
    S_GAMING_GROUP, S_GAMING, S_GAMES_HINT, S_GAMES_CUE, S_PAUSED_FOR, S_PICK, S_NO_APPS, S_OPEN_LOG,
    S_COUNT
};

static const WCHAR *const STR[LANG_COUNT][S_COUNT] = {
    [LANG_EN] = {
        [S_MINI_BREAK]    = L"Mini break",
        [S_BREAK]         = L"Break",
        [S_MINI_BREAK_LC] = L"mini break",
        [S_BREAK_LC]      = L"break",
        [S_NEXT_FMT]      = L"Next %s in %s",
        [S_IN_MIN_FMT]    = L"%s in %u min",
        [S_IN_SEC_FMT]    = L"%s in %u s",
        [S_IN_CLOCK_FMT]  = L"%s in %s",
        [S_MINI_RUNNING]  = L"Mini break in progress",
        [S_BREAK_RUNNING] = L"Break in progress",
        [S_PAUSED]        = L"Paused while the PC is locked",
        [S_PAUSED_IDLE]   = L"Paused: the PC is idle",
        [S_NONE]          = L"No breaks enabled",
        [S_TAKE_MINI]     = L"Take a mini break now",
        [S_TAKE_LONG]     = L"Take a break now",
        [S_SETTINGS_MENU] = L"Settings...",
        [S_EXIT]          = L"Exit",
        [S_DLG_TITLE]     = L"Pauserick Settings",
        [S_ENABLE_MINI]   = L"Enable mini breaks",
        [S_ENABLE_LONG]   = L"Enable breaks",
        [S_EVERY]         = L"Every",
        [S_DURATION]      = L"Duration",
        [S_OFF]           = L"Off",
        [S_GENERAL]       = L"General",
        [S_AUTOSTART]     = L"Start with Windows",
        [S_STRICT]        = L"Block Task Manager during breaks",
        [S_SOUND]         = L"Sound when a break ends",
        [S_WARN]          = L"Warn before",
        [S_IDLE]          = L"Pause if idle for",
        [S_WHEN_BACK]     = L"When back",
        [S_RESUME_RESTART]  = L"Restart the timers from zero",
        [S_RESUME_CONTINUE] = L"Continue with the time left",
        [S_MINI_DESC]     = L"Short, to rest your eyes",
        [S_LONG_DESC]     = L"Longer, to get up and move",
        [S_IDLE_GROUP]    = L"Inactivity",
        [S_IDLE_DESC]     = L"Stops while you are away",
        [S_AFTER]         = L"After",
        [S_GAMING_DESC]   = L"No breaks while you play",
        [S_GAMES_AUTO]    = L"Games from Steam, Epic, GOG, Xbox, EA, Ubisoft, Riot and Rockstar, and anything in "
                            L"exclusive fullscreen, are recognized automatically.",
        [S_STRICT_HINT]   = L"Asks for admin rights once",
        [S_AUTOUPDATE]    = L"Automatic updates",
        [S_UPD_CHECK]     = L"Check for updates",
        [S_GITHUB]        = L"Project page on GitHub",
        [S_UPD_CHECKING]  = L"Checking...",
        [S_UPD_LATEST]    = L"You have the latest version",
        [S_UPD_CHECK_FAILED] = L"Check failed: no connection?",
        [S_UPD_GET_FMT]   = L"Install version %s",
        [S_UPD_TRAY_FMT]  = L"Install update %s...",
        [S_UPD_TITLE_FMT] = L"Pauserick %s is available",
        [S_UPD_TEXT_FMT]  = L"You have version %s. The new version is checked against the developer's digital "
                            L"signature before it is installed.",
        [S_UPD_INSTALL]   = L"Install now\nDownloads it, checks the signature and restarts Pauserick",
        [S_UPD_LATER]     = L"Not now\nAsk me again tomorrow",
        [S_UPD_SKIP]      = L"Skip this version\nWait for the next one",
        [S_UPD_NOTES]     = L"What's new",
        [S_UPD_HIDE_NOTES] = L"Hide what's new",
        [S_UPD_FAILED_FMT] = L"The update was not installed: %s.\n\nPauserick keeps running the current version.",
        [S_UPD_E_NET]     = L"the download failed",
        [S_UPD_E_SIG]     = L"its digital signature is not valid, so it was discarded",
        [S_UPD_E_FILE]    = L"the program file could not be replaced",
        [S_UPD_E_RIGHTS]  = L"administrator rights were not granted",
        [S_LANGUAGE]      = L"Language",
        [S_CLOSE]         = L"Close",
        [S_ERR_STRICT_ON] = L"Blocking Task Manager needs administrator rights, which were not granted.",
        [S_ERR_STRICT_OFF]= L"Task Manager blocking is still on: administrator rights are needed to turn it off.",
        [S_ERR_AUTOSTART] = L"The startup setting was not changed: administrator rights were not granted.",
        [S_GAMING_GROUP]  = L"Gaming mode",
        [S_GAMING]        = L"Gaming mode: no breaks while a game is in the foreground",
        [S_GAMES_HINT]    = L"Other games, separated by ;",
        [S_GAMES_CUE]     = L"e.g. Overwatch.exe; RetroArch.exe",
        [S_PAUSED_FOR]    = L"Pauserick paused for process %s",
        [S_PICK]          = L"Choose app...",
        [S_NO_APPS]       = L"No open apps found",
        [S_OPEN_LOG]      = L"Open log file",
    },
    [LANG_IT] = {
        [S_MINI_BREAK]    = L"Mini pausa",
        [S_BREAK]         = L"Pausa",
        [S_MINI_BREAK_LC] = L"mini pausa",
        [S_BREAK_LC]      = L"pausa",
        [S_NEXT_FMT]      = L"Prossima %s tra %s",
        [S_IN_MIN_FMT]    = L"%s tra %u min",
        [S_IN_SEC_FMT]    = L"%s tra %u s",
        [S_IN_CLOCK_FMT]  = L"%s tra %s",
        [S_MINI_RUNNING]  = L"Mini pausa in corso",
        [S_BREAK_RUNNING] = L"Pausa in corso",
        [S_PAUSED]        = L"In attesa: il PC è bloccato",
        [S_PAUSED_IDLE]   = L"In pausa: il PC è inattivo",
        [S_NONE]          = L"Nessuna pausa attiva",
        [S_TAKE_MINI]     = L"Fai una mini pausa adesso",
        [S_TAKE_LONG]     = L"Fai una pausa adesso",
        [S_SETTINGS_MENU] = L"Impostazioni...",
        [S_EXIT]          = L"Esci",
        [S_DLG_TITLE]     = L"Impostazioni di Pauserick",
        [S_ENABLE_MINI]   = L"Attiva le mini pause",
        [S_ENABLE_LONG]   = L"Attiva le pause",
        [S_EVERY]         = L"Ogni",
        [S_DURATION]      = L"Durata",
        [S_OFF]           = L"Disattivato",
        [S_GENERAL]       = L"Generale",
        [S_AUTOSTART]     = L"Avvia con Windows",
        [S_STRICT]        = L"Blocca Task Manager durante le pause",
        [S_SOUND]         = L"Suono alla fine della pausa",
        [S_WARN]          = L"Preavviso",
        [S_IDLE]          = L"Pausa se inattivo per",
        [S_WHEN_BACK]     = L"Al ritorno",
        [S_RESUME_RESTART]  = L"Riparti da capo con il timer",
        [S_RESUME_CONTINUE] = L"Continua con i minuti rimanenti",
        [S_MINI_DESC]     = L"Breve, per riposare gli occhi",
        [S_LONG_DESC]     = L"Più lunga, per alzarti e muoverti",
        [S_IDLE_GROUP]    = L"Inattività",
        [S_IDLE_DESC]     = L"Si ferma se non usi il PC",
        [S_AFTER]         = L"Dopo",
        [S_GAMING_DESC]   = L"Niente pause mentre giochi",
        [S_GAMES_AUTO]    = L"I giochi di Steam, Epic, GOG, Xbox, EA, Ubisoft, Riot e Rockstar, e tutto ciò che "
                            L"va a schermo intero esclusivo, sono riconosciuti da soli.",
        [S_STRICT_HINT]   = L"Chiede i diritti di admin una volta",
        [S_AUTOUPDATE]    = L"Aggiornamenti automatici",
        [S_UPD_CHECK]     = L"Cerca aggiornamenti",
        [S_GITHUB]        = L"Pagina del progetto su GitHub",
        [S_UPD_CHECKING]  = L"Controllo in corso...",
        [S_UPD_LATEST]    = L"Hai l'ultima versione",
        [S_UPD_CHECK_FAILED] = L"Controllo non riuscito: sei offline?",
        [S_UPD_GET_FMT]   = L"Installa la versione %s",
        [S_UPD_TRAY_FMT]  = L"Installa l'aggiornamento %s...",
        [S_UPD_TITLE_FMT] = L"È disponibile Pauserick %s",
        [S_UPD_TEXT_FMT]  = L"Hai la versione %s. Prima di essere installata, la nuova versione viene verificata "
                            L"con la firma digitale dello sviluppatore.",
        [S_UPD_INSTALL]   = L"Installa ora\nLa scarica, verifica la firma e riavvia Pauserick",
        [S_UPD_LATER]     = L"Non ora\nChiedimelo di nuovo domani",
        [S_UPD_SKIP]      = L"Salta questa versione\nAspetta la prossima",
        [S_UPD_NOTES]     = L"Novità",
        [S_UPD_HIDE_NOTES] = L"Nascondi le novità",
        [S_UPD_FAILED_FMT] = L"L'aggiornamento non è stato installato: %s.\n\nPauserick continua con la versione attuale.",
        [S_UPD_E_NET]     = L"il download non è riuscito",
        [S_UPD_E_SIG]     = L"la firma digitale non è valida, quindi è stato scartato",
        [S_UPD_E_FILE]    = L"non è stato possibile sostituire il file del programma",
        [S_UPD_E_RIGHTS]  = L"i diritti di amministratore non sono stati concessi",
        [S_LANGUAGE]      = L"Lingua",
        [S_CLOSE]         = L"Chiudi",
        [S_ERR_STRICT_ON] = L"Per bloccare Task Manager servono i diritti di amministratore, che non sono stati concessi.",
        [S_ERR_STRICT_OFF]= L"Il blocco di Task Manager è ancora attivo: per disattivarlo servono i diritti di amministratore.",
        [S_ERR_AUTOSTART] = L"L'avvio con Windows non \u00e8 stato modificato: i diritti di amministratore non sono stati concessi.",
        [S_GAMING_GROUP]  = L"Modalit\u00e0 gaming",
        [S_GAMING]        = L"Modalit\u00e0 gaming: nessuna pausa mentre un gioco \u00e8 in primo piano",
        [S_GAMES_HINT]    = L"Altri giochi, separati da ;",
        [S_GAMES_CUE]     = L"es. Overwatch.exe; RetroArch.exe",
        [S_PAUSED_FOR]    = L"Pauserick in pausa per il processo %s",
        [S_PICK]          = L"Scegli app...",
        [S_NO_APPS]       = L"Nessuna app aperta trovata",
        [S_OPEN_LOG]      = L"Apri il file di log",
    },
};

#define T(id) STR[g_cfg.lang][id]

/* limits: { min, max } */
#define LIM_MINI_EVERY  1, 1440
#define LIM_MINI_DUR    5, 3600
#define LIM_LONG_EVERY  1, 1440
#define LIM_LONG_DUR    1, 240
#define LIM_WARN        0, 600
#define LIM_IDLE        1, 240

typedef struct {
    HWND hwnd;
    HFONT fTitle, fTime;
    RECT rcTitle, rcTime;
} Overlay;

static HINSTANCE g_inst;
static HWND      g_main, g_settings, g_warn;
static UINT      g_msgTaskbarCreated;
static NOTIFYICONDATAW g_nid;
static int       g_trayAdded;
static WCHAR     g_ini[MAX_PATH];
static WCHAR     g_exe[MAX_PATH];
static int       g_customIni, g_firstRun, g_elevated, g_fromTask;
static HANDLE    g_mutex;
static Settings  g_cfg;

static int       g_state = ST_IDLE;
static int       g_breakType, g_warnType;
static ULONGLONG g_miniRef, g_longRef, g_breakEnd, g_warnAt;
static int       g_pauseFlags;     /* PAUSE_*: why the timers are stopped right now */
static ULONGLONG g_pausedSince;    /* the timers stopped then (first reason of the current pause) */
static int       g_pauseWasIdle;   /* the current pause includes an idle pause */
static ULONGLONG g_activeSince;    /* last unlock / wake-up / break end: idle time counts from here at most */
static int       g_gamingNow;      /* gaming mode is holding the breaks (last check) */
static WCHAR     g_pauseProc[64];  /* ...because of this process (exe name) */

#define STATUS_CCH 192 /* buffers for fmt_status(): longest text + a 63-char exe name fits */
static LONGLONG  g_lastShownSecs = -1;

static Overlay   g_ov[MAX_OVERLAYS];
static int       g_show = 1; /* 0 only in the test build's self-test: windows stay hidden */
static int       g_nov;

static HFONT     g_warnFont;
static UINT      g_warnDpi = 96;

static HANDLE    g_hookThread;
static DWORD     g_hookThreadId;
static volatile LONG g_taskMgrLocked;   /* we set DisableTaskMgr and must remove it */
static volatile LONGLONG g_blockUntil;  /* input hooks block only before this tick (watchdog) */

static int       g_updPrompt;   /* a newer version waits to be offered when nothing else is going on */
static int       g_updRestart;  /* an update was installed during a break: restart when it ends */
static int update_apply(const WCHAR *exeUrl, const WCHAR *sigUrl, const WCHAR *version);
static void update_restart(void);

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static ULONGLONG now_ms(void) { return GetTickCount64(); }

static int ini_int(const WCHAR *sec, const WCHAR *key, int def, int lo, int hi)
{
    return clampi((int)GetPrivateProfileIntW(sec, key, def, g_ini), lo, hi);
}

static void ini_put(const WCHAR *sec, const WCHAR *key, int v)
{
    WCHAR b[16];
    wsprintfW(b, L"%d", v);
    WritePrivateProfileStringW(sec, key, b, g_ini);
}

static void settings_load(void)
{
    /* Defaults: a 30 s mini break every 30 min, a 3 min break every hour, pause after 3 idle minutes. */
    g_cfg.miniEnabled    = ini_int(L"MiniBreak", L"Enabled", 1, 0, 1);
    g_cfg.miniEveryMin   = ini_int(L"MiniBreak", L"EveryMinutes", 30, LIM_MINI_EVERY);
    g_cfg.miniDurSec     = ini_int(L"MiniBreak", L"DurationSeconds", 30, LIM_MINI_DUR);
    g_cfg.longEnabled    = ini_int(L"Break", L"Enabled", 1, 0, 1);
    g_cfg.longEveryMin   = ini_int(L"Break", L"EveryMinutes", 60, LIM_LONG_EVERY);
    g_cfg.longDurMin     = ini_int(L"Break", L"DurationMinutes", 3, LIM_LONG_DUR);
    g_cfg.warnSec        = ini_int(L"General", L"WarnSeconds", 0, LIM_WARN);
    /* Up to 1.2 the only idle setting was IdleResetMinutes (0 = off): keep "off" if it was off. */
    int oldIdle          = (int)GetPrivateProfileIntW(L"General", L"IdleResetMinutes", -1, g_ini);
    g_cfg.idlePause      = ini_int(L"General", L"IdlePause", oldIdle == 0 ? 0 : 1, 0, 1);
    g_cfg.idlePauseMin   = ini_int(L"General", L"IdlePauseMinutes", 3, LIM_IDLE);
    g_cfg.idleContinue   = ini_int(L"General", L"IdleContinue", 0, 0, 1);
    g_cfg.sound          = ini_int(L"General", L"SoundAtEnd", 1, 0, 1);
    g_cfg.lang           = ini_int(L"General", L"Language", LANG_EN, 0, LANG_COUNT - 1);
    g_cfg.gaming         = ini_int(L"Gaming", L"Enabled", 1, 0, 1);
    g_cfg.autoUpdate     = ini_int(L"Update", L"Auto", 1, 0, 1);
    GetPrivateProfileStringW(L"Gaming", L"Games", L"", g_cfg.games, ARRAYSIZE(g_cfg.games), g_ini);
}

static void settings_save(void)
{
    ini_put(L"MiniBreak", L"Enabled", g_cfg.miniEnabled);
    ini_put(L"MiniBreak", L"EveryMinutes", g_cfg.miniEveryMin);
    ini_put(L"MiniBreak", L"DurationSeconds", g_cfg.miniDurSec);
    ini_put(L"Break", L"Enabled", g_cfg.longEnabled);
    ini_put(L"Break", L"EveryMinutes", g_cfg.longEveryMin);
    ini_put(L"Break", L"DurationMinutes", g_cfg.longDurMin);
    ini_put(L"General", L"WarnSeconds", g_cfg.warnSec);
    ini_put(L"General", L"IdlePause", g_cfg.idlePause);
    ini_put(L"General", L"IdlePauseMinutes", g_cfg.idlePauseMin);
    ini_put(L"General", L"IdleContinue", g_cfg.idleContinue);
    WritePrivateProfileStringW(L"General", L"IdleResetMinutes", NULL, g_ini); /* replaced by the three above */
    ini_put(L"General", L"SoundAtEnd", g_cfg.sound);
    ini_put(L"General", L"Language", g_cfg.lang);
    ini_put(L"Gaming", L"Enabled", g_cfg.gaming);
    ini_put(L"Update", L"Auto", g_cfg.autoUpdate);
    WritePrivateProfileStringW(L"Gaming", L"Games", g_cfg.games, g_ini);
}

/* ------------------------------------------------------------------- log */
/*
 * pauserick.log next to settings.ini: plain UTF-8 text, one line per event
 * (state changes only, never per tick). Every line opens, appends and closes the
 * file, so it is never locked, is safe from any thread (also the watchdog and the
 * crash handler) and nothing is lost on a crash. Over LOG_MAX_BYTES the file
 * becomes pauserick.old.log (replacing the previous old log, also ours).
 */
#define LOG_MAX_BYTES (512 * 1024)

static WCHAR g_logPath[MAX_PATH], g_logOld[MAX_PATH];

static void log_init(void)
{
    WCHAR dir[MAX_PATH];
    lstrcpynW(dir, g_ini, ARRAYSIZE(dir));
    WCHAR *slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    if (lstrlenW(dir) > MAX_PATH - 24) return;
    wsprintfW(g_logPath, L"%s\\pauserick.log", dir);
    wsprintfW(g_logOld, L"%s\\pauserick.old.log", dir);
}

static void log_write(const WCHAR *level, const WCHAR *fmt, va_list ap)
{
    if (!g_logPath[0]) return;
    WCHAR msg[1025], line[1100];
    wvsprintfW(msg, fmt, ap);          /* wvsprintfW never writes more than 1024 chars + NUL */
    msg[900] = 0;                      /* leave room for the prefix below */
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = wsprintfW(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %-5s  %s\r\n", t.wYear, t.wMonth, t.wDay,
                      t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, msg);
    char utf8[3400];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof utf8, NULL, NULL);
    if (len <= 0) return;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, share, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > LOG_MAX_BYTES) {
        CloseHandle(f);
        MoveFileExW(g_logPath, g_logOld, MOVEFILE_REPLACE_EXISTING);
        f = CreateFileW(g_logPath, FILE_APPEND_DATA, share, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) return;
    }
    DWORD w;
    WriteFile(f, utf8, (DWORD)len, &w, NULL);
    CloseHandle(f);
}

static void log_info(const WCHAR *fmt, ...)  { va_list ap; va_start(ap, fmt); log_write(L"INFO", fmt, ap);  va_end(ap); }
static void log_warn(const WCHAR *fmt, ...)  { va_list ap; va_start(ap, fmt); log_write(L"WARN", fmt, ap);  va_end(ap); }
static void log_error(const WCHAR *fmt, ...) { va_list ap; va_start(ap, fmt); log_write(L"ERROR", fmt, ap); va_end(ap); }

static const WCHAR *onoff(int v) { return v ? L"on" : L"off"; }
static const WCHAR *resume_name(int cont) { return cont ? L"continue with the time left" : L"restart from zero"; }

static void log_settings(const WCHAR *prefix, const Settings *s)
{
    log_info(L"%s: mini break %s every %d min for %d s | break %s every %d min for %d min | warning %d s | "
             L"pause if idle %s, after %d min, when back %s | sound %s | gaming mode %s | other games '%s' | language %s | "
             L"daily update check %s",
             prefix, onoff(s->miniEnabled), s->miniEveryMin, s->miniDurSec, onoff(s->longEnabled), s->longEveryMin,
             s->longDurMin, s->warnSec, onoff(s->idlePause), s->idlePauseMin, resume_name(s->idleContinue),
             onoff(s->sound), onoff(s->gaming), s->games, s->lang == LANG_IT ? L"Italiano" : L"English",
             onoff(s->autoUpdate));
}

/* one line per changed setting: "Setting changed: mini break every 30 -> 45 min" */
static void log_settings_diff(const Settings *a, const Settings *b)
{
#define LOG_FLAG(f, label) \
    if (a->f != b->f) log_info(L"Setting changed: %s %s -> %s", label, onoff(a->f), onoff(b->f))
#define LOG_NUM(f, label, unit) \
    if (a->f != b->f) log_info(L"Setting changed: %s %d -> %d %s", label, a->f, b->f, unit)
    LOG_FLAG(miniEnabled, L"mini breaks");
    LOG_NUM(miniEveryMin, L"mini break every", L"min");
    LOG_NUM(miniDurSec, L"mini break duration", L"s");
    LOG_FLAG(longEnabled, L"breaks");
    LOG_NUM(longEveryMin, L"break every", L"min");
    LOG_NUM(longDurMin, L"break duration", L"min");
    LOG_NUM(warnSec, L"warning before a break", L"s");
    LOG_FLAG(idlePause, L"pause if idle");
    LOG_NUM(idlePauseMin, L"pause if idle for", L"min");
    if (a->idleContinue != b->idleContinue)
        log_info(L"Setting changed: when back from an idle pause %s -> %s", resume_name(a->idleContinue),
                 resume_name(b->idleContinue));
    LOG_FLAG(sound, L"sound at the end");
    LOG_FLAG(gaming, L"gaming mode");
    LOG_FLAG(autoUpdate, L"daily update check");
    if (lstrcmpW(a->games, b->games)) log_info(L"Setting changed: other games '%s' -> '%s'", a->games, b->games);
#undef LOG_FLAG
#undef LOG_NUM
}

/* ------------------------------------------------- start with Windows */

static const WCHAR RUN_KEY[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR APPROVED_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

/* Normal mode autostart: an HKCU Run entry (no admin rights needed). */
static int autostart_get(void)
{
    WCHAR buf[MAX_PATH * 2];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_NAME, RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return 0;
    /* Task Manager's "Startup apps" toggle stores an odd first byte when disabled. */
    BYTE ap[32];
    sz = sizeof ap;
    if (RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_NAME, RRF_RT_REG_BINARY, NULL, ap, &sz) == ERROR_SUCCESS &&
        sz >= 1 && (ap[0] & 1))
        return 0;
    return 1;
}

static void autostart_set(int on)
{
    log_info(L"Start with Windows (startup entry) %s", onoff(on));
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_NAME);
        return;
    }
    WCHAR cmd[MAX_PATH * 2 + 32];
    if (g_customIni) wsprintfW(cmd, L"\"%s\" --config \"%s\"", g_exe, g_ini);
    else             wsprintfW(cmd, L"\"%s\"", g_exe);
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_NAME, REG_SZ, cmd, (DWORD)(lstrlenW(cmd) + 1) * sizeof(WCHAR));
    RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_NAME);
}

/* If the exe was moved, point the existing autostart entry at the new location. */
static void autostart_fix_path(void)
{
    WCHAR buf[MAX_PATH * 2];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_NAME, RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return;
    WCHAR *p = buf, *e;
    if (*p == L'"') p++;
    for (e = p; *e && *e != L'"'; e++) {}
    *e = 0;
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) autostart_set(1);
}

/* ------------------------------------------ strict mode (elevated task) */
/*
 * A normal-privilege app cannot disable Task Manager, and its input hooks do not
 * see keystrokes aimed at elevated windows. Strict mode registers a logon task
 * with "highest privileges" (one UAC prompt), so Pauserick runs elevated from
 * then on, without further prompts.
 */

static void strict_task_exe(WCHAR *out)
{
    GetPrivateProfileStringW(L"State", L"StrictTaskExe", L"", out, MAX_PATH, g_ini);
}

static int strict_registered(void)
{
    WCHAR p[MAX_PATH];
    strict_task_exe(p);
    return p[0] != 0;
}

static int strict_matches_exe(void)
{
    WCHAR p[MAX_PATH];
    strict_task_exe(p);
    return p[0] && !lstrcmpiW(p, g_exe);
}

static int is_elevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION e = { 0 };
    DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    GetTokenInformation(tok, TokenElevation, &e, sizeof e, &len);
    CloseHandle(tok);
    return e.TokenIsElevated != 0;
}

/* Runs schtasks.exe without any window, elevated (UAC) only when needed;
 * returns its exit code or -1. */
static DWORD schtasks(const WCHAR *args, BOOL elevate)
{
    WCHAR exe[MAX_PATH];
    HANDLE proc = NULL;
    GetSystemDirectoryW(exe, MAX_PATH - 16);
    lstrcatW(exe, L"\\schtasks.exe");
    if (elevate && !g_elevated) {
        SHELLEXECUTEINFOW sei = { sizeof sei };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.hwnd = g_settings;
        sei.lpVerb = L"runas";
        sei.lpFile = exe;
        sei.lpParameters = args;
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei)) proc = sei.hProcess; /* fails if UAC is declined */
    } else {
        WCHAR cmd[1024];
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi;
        wsprintfW(cmd, L"\"%s\" %s", exe, args);
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            proc = pi.hProcess;
        }
    }
    if (!proc) return (DWORD)-1;
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(proc, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (g_settings) SetForegroundWindow(g_settings); /* keep the settings window active */
    return code;
}

typedef struct { WCHAR *p; int left; } Buf;

static void put(Buf *b, const WCHAR *s, int xmlEscape)
{
    for (; *s && b->left > 8; s++) {
        const WCHAR *r = NULL;
        if (xmlEscape) {
            if (*s == L'&') r = L"&amp;";
            else if (*s == L'<') r = L"&lt;";
            else if (*s == L'>') r = L"&gt;";
            else if (*s == L'"') r = L"&quot;";
        }
        if (!r) { *b->p++ = *s; b->left--; continue; }
        for (; *r; r++) { *b->p++ = *r; b->left--; }
    }
    *b->p = 0;
}

/* The SID of the user Pauserick runs for, as text (the task starts Pauserick for this user). */
static int current_user_sid(WCHAR *out, int cch)
{
    HANDLE tok;
    out[0] = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BYTE ub[256];
        DWORD len;
        LPWSTR s;
        if (GetTokenInformation(tok, TokenUser, ub, sizeof ub, &len) &&
            ConvertSidToStringSidW(((TOKEN_USER *)ub)->User.Sid, &s)) {
            lstrcpynW(out, s, cch);
            LocalFree(s);
        }
        CloseHandle(tok);
    }
    return out[0] != 0;
}

/* "...\Pauserick.exe" -> "...\Pauserick<suffix>" (our own files next to the exe) */
static void exe_sibling(WCHAR *out, const WCHAR *exe, const WCHAR *suffix)
{
    lstrcpynW(out, exe, MAX_PATH - 16);
    int n = lstrlenW(out);
    if (n > 4 && !lstrcmpiW(out + n - 4, L".exe")) out[n - 4] = 0;
    lstrcatW(out, suffix);
}

/*
 * Where Block Task Manager keeps Pauserick: a folder only administrators can change
 * (Program Files). The task starts this copy with administrator rights, so nothing
 * running without them can swap the exe to get those rights. The location comes from
 * the system's known-folder registry, never from environment variables a program
 * could change. The test build uses a folder of its own instead.
 */
static int strict_install_path(WCHAR *dir, WCHAR *exe)
{
#ifdef PAUSERICK_TEST_TASK
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH - 40);
    if (!n || n >= MAX_PATH - 40) return 0;
    lstrcatW(dir, L"\\PauserickTest");
    if (exe) wsprintfW(exe, L"%s\\PauserickTest.exe", dir);
#else
    static const GUID programFiles = { 0x905e63b6, 0xc1bf, 0x494e, { 0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a } };
    PWSTR p = NULL;
    if (FAILED(SHGetKnownFolderPath(&programFiles, 0, NULL, &p)) || !p) return 0;
    int ok = lstrlenW(p) < MAX_PATH - 40;
    if (ok) {
        wsprintfW(dir, L"%s\\Pauserick", p);
        if (exe) wsprintfW(exe, L"%s\\Pauserick.exe", dir);
    }
    CoTaskMemFree(p);
    if (!ok) return 0;
#endif
    return 1;
}

/* The task already starts the copy in the protected folder. */
static int strict_at_install_path(void)
{
    WCHAR cur[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH];
    strict_task_exe(cur);
    return cur[0] && strict_install_path(dir, exe) && !lstrcmpiW(cur, exe);
}

/* logon = whether the task also starts Pauserick at logon ("Start with Windows").
 * Without it the task is still used to launch Pauserick elevated on demand. */
static int task_write_xml(const WCHAR *path, int logon, const WCHAR *exe, const WCHAR *sid)
{
    WCHAR args[MAX_PATH + 48] = L"--from-task";
    if (!sid[0]) return 0;
    if (g_customIni) { lstrcatW(args, L" --config \""); lstrcatW(args, g_ini); lstrcatW(args, L"\""); }

    static WCHAR xml[8192];
    Buf b = { xml, ARRAYSIZE(xml) };
    put(&b, L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
            L"  <RegistrationInfo>\r\n"
            L"    <Description>Starts Pauserick at logon (strict mode).</Description>\r\n"
            L"    <SecurityDescriptor>D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;" TASK_USER_RIGHTS L";;;", 0);
    put(&b, sid, 0);
    put(&b, L")</SecurityDescriptor>\r\n"
            L"  </RegistrationInfo>\r\n"
            L"  <Triggers>\r\n"
            L"    <LogonTrigger>\r\n"
            L"      <Enabled>", 0);
    put(&b, logon ? L"true" : L"false", 0);
    put(&b, L"</Enabled>\r\n"
            L"      <UserId>", 0);
    put(&b, sid, 0);
    put(&b, L"</UserId>\r\n"
            L"    </LogonTrigger>\r\n"
            L"  </Triggers>\r\n"
            L"  <Principals>\r\n"
            L"    <Principal id=\"Author\">\r\n"
            L"      <UserId>", 0);
    put(&b, sid, 0);
    put(&b, L"</UserId>\r\n"
            L"      <LogonType>InteractiveToken</LogonType>\r\n"
            L"      <RunLevel>" TASK_RUNLEVEL L"</RunLevel>\r\n"
            L"    </Principal>\r\n"
            L"  </Principals>\r\n"
            L"  <Settings>\r\n"
            L"    <MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy>\r\n"
            L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
            L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
            L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
            L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
            L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
            L"    <IdleSettings>\r\n"
            L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n"
            L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
            L"    </IdleSettings>\r\n"
            L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
            L"    <Enabled>true</Enabled>\r\n"
            L"    <Hidden>false</Hidden>\r\n"
            L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
            L"    <WakeToRun>false</WakeToRun>\r\n"
            L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
            L"    <Priority>7</Priority>\r\n"
            L"  </Settings>\r\n"
            L"  <Actions Context=\"Author\">\r\n"
            L"    <Exec>\r\n"
            L"      <Command>", 0);
    put(&b, exe, 1);
    put(&b, L"</Command>\r\n"
            L"      <Arguments>", 0);
    put(&b, args, 1);
    put(&b, L"</Arguments>\r\n"
            L"    </Exec>\r\n"
            L"  </Actions>\r\n"
            L"</Task>\r\n", 0);

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WCHAR bom = 0xFEFF;
    DWORD w;
    BOOL ok = WriteFile(f, &bom, sizeof bom, &w, NULL) &&
              WriteFile(f, xml, (DWORD)(b.p - xml) * sizeof(WCHAR), &w, NULL);
    CloseHandle(f);
    return ok;
}

/*
 * Runs with administrator rights (the elevated helper, or an elevated instance): copies
 * this exe into the protected folder and points the elevated logon task at that copy.
 * The task file is written in that folder too, never in the user's %TEMP%, so nothing
 * can change it between writing and registering.
 */
static int strict_install_elevated(int logon, const WCHAR *sid)
{
    WCHAR dir[MAX_PATH], exe[MAX_PATH], old[MAX_PATH], xml[MAX_PATH + 16], args[MAX_PATH + 64];
    if (!strict_install_path(dir, exe)) return 0;
    CreateDirectoryW(dir, NULL); /* inherits Program Files' permissions: only administrators may write */
    if (lstrcmpiW(g_exe, exe)) {
        if (!CopyFileW(g_exe, exe, FALSE)) {
            /* a running copy locks the file: move it aside (removed at the next start) */
            exe_sibling(old, exe, L".old.exe");
            if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING) || !CopyFileW(g_exe, exe, FALSE)) {
                log_error(L"Could not copy Pauserick to %s (error %lu)", exe, GetLastError());
                return 0;
            }
        }
        log_info(L"Pauserick copied to %s", exe);
    }
    wsprintfW(xml, L"%s\\task.xml.tmp", dir);
    int ok = task_write_xml(xml, logon, exe, sid);
    if (ok) {
        wsprintfW(args, L"/create /tn \"" TASK_NAME L"\" /xml \"%s\" /f", xml);
        DWORD rc = schtasks(args, FALSE); /* we already have the rights */
        if (rc != 0) log_error(L"Startup task not created: schtasks exit code %lu", rc);
        ok = rc == 0;
    }
    DeleteFileW(xml);
    if (!ok) return 0;
    WritePrivateProfileStringW(L"State", L"StrictTaskExe", exe, g_ini);
    WritePrivateProfileStringW(L"State", L"StrictAutostart", logon ? L"1" : L"0", g_ini);
    log_info(L"Startup task '%s' created for %s (Block Task Manager on, start with Windows %s)", TASK_NAME, exe,
             onoff(logon));
    return 1;
}

/* Starts this exe again with administrator rights (one UAC prompt) and waits for it;
 * returns its exit code, or -1 if it did not run (prompt declined). The test build
 * runs it without elevation, so the same flow can be tested without UAC. */
static DWORD run_self_elevated(const WCHAR *params, DWORD timeoutMs)
{
    HANDLE proc = NULL;
    if (TASK_ELEVATE) {
        SHELLEXECUTEINFOW sei = { sizeof sei };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.hwnd = g_settings;
        sei.lpVerb = L"runas";
        sei.lpFile = g_exe;
        sei.lpParameters = params;
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei)) proc = sei.hProcess;
    } else {
        WCHAR cmd[2048];
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi;
        wsprintfW(cmd, L"\"%s\" %s", g_exe, params);
        if (CreateProcessW(g_exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            proc = pi.hProcess;
        }
    }
    if (!proc) return (DWORD)-1;
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(proc, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (g_settings) SetForegroundWindow(g_settings);
    return code;
}

/* Turns Block Task Manager on: Pauserick is installed in the protected folder and the
 * elevated task points there. Without administrator rights it asks for them once. */
static int strict_install(int logon)
{
    WCHAR sid[200], params[MAX_PATH + 300];
    if (!current_user_sid(sid, ARRAYSIZE(sid))) return 0;
    if (g_elevated) return strict_install_elevated(logon, sid);
    wsprintfW(params, L"--strict-install %d %s %d --config \"%s\"", logon, sid, g_customIni, g_ini);
    DWORD rc = run_self_elevated(params, 60000);
    if (rc == (DWORD)-1) log_warn(L"Block Task Manager not set up: administrator rights were not granted");
    else if (rc != 0)    log_error(L"Block Task Manager not set up (error %lu, see the lines above)", rc);
    return rc == 0;
}

static int task_delete(void)
{
    if (schtasks(L"/delete /tn \"" TASK_NAME L"\" /f", TASK_ELEVATE) != 0 &&
        schtasks(L"/query /tn \"" TASK_NAME L"\"", FALSE) == 0) {
        log_warn(L"Startup task not removed: administrator rights were not granted");
        return 0; /* still there (e.g. UAC declined) */
    }
    WritePrivateProfileStringW(L"State", L"StrictTaskExe", NULL, g_ini);
    WritePrivateProfileStringW(L"State", L"StrictAutostart", NULL, g_ini);
    log_info(L"Startup task '%s' removed (Block Task Manager off)", TASK_NAME);
    return 1;
}

/* "Start with Windows" as the user sees it, in either mode. */
static int startup_enabled(void)
{
    if (strict_registered()) return GetPrivateProfileIntW(L"State", L"StrictAutostart", 1, g_ini) != 0;
    return autostart_get();
}

static int task_run(void)
{
    return schtasks(L"/run /tn \"" TASK_NAME L"\"", FALSE) == 0;
}

/* ------------------------------------------- Task Manager lock (strict) */

static const WCHAR POLICY_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";

/* Pauserick writes exactly one registry value here (DisableTaskMgr = 1), only if it
 * did not exist, and removes only that: if the value is gone or was changed by
 * someone else meanwhile, it is left alone. Nothing else is ever deleted. */
static void taskmgr_lock(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, POLICY_KEY, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExW(k, L"DisableTaskMgr", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND) {
        DWORD one = 1;
        WritePrivateProfileStringW(L"State", L"TaskManagerLocked", L"1", g_ini); /* crash recovery marker */
        LSTATUS r = RegSetValueExW(k, L"DisableTaskMgr", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
        if (r == ERROR_SUCCESS) {
            InterlockedExchange(&g_taskMgrLocked, 1);
            log_info(L"Task Manager disabled for the break");
        } else {
            WritePrivateProfileStringW(L"State", L"TaskManagerLocked", L"0", g_ini);
            log_warn(L"Could not disable Task Manager (error %ld)", (long)r);
        }
    } else {
        log_info(L"Task Manager is already disabled by a policy: left as it is");
    }
    RegCloseKey(k);
}

/* Removes our DisableTaskMgr value if it is still the one we wrote. 1 = done. */
static int taskmgr_remove_ours(void)
{
    HKEY k;
    LSTATUS r = RegOpenKeyExW(HKEY_CURRENT_USER, POLICY_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k);
    if (r == ERROR_FILE_NOT_FOUND) return 1;
    if (r != ERROR_SUCCESS) return 0;
    DWORD type = 0, val = 0, sz = sizeof val;
    r = RegQueryValueExW(k, L"DisableTaskMgr", NULL, &type, (BYTE *)&val, &sz);
    int done = 1;
    if (r == ERROR_SUCCESS && type == REG_DWORD && val == 1)
        done = RegDeleteValueW(k, L"DisableTaskMgr") == ERROR_SUCCESS;
    RegCloseKey(k);
    return done;
}

/* Safe to call from any thread, any number of times (also from the crash handler). */
static void taskmgr_unlock(void)
{
    if (!InterlockedExchange(&g_taskMgrLocked, 0)) return;
    if (taskmgr_remove_ours()) {
        WritePrivateProfileStringW(L"State", L"TaskManagerLocked", L"0", g_ini);
        log_info(L"Task Manager restored");
    } else { /* the marker stays and the next start retries */
        log_warn(L"Could not restore Task Manager yet: will retry at the next start");
    }
}

/* --------------------------------------------------------- input blocking */

/* Watchdog: past the deadline the hooks let everything through, whatever the
 * rest of the program is doing, so the PC can never stay locked. */
static int blocking_now(void) { return GetTickCount64() < (ULONGLONG)g_blockUntil; }

static LRESULT CALLBACK kbd_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && blocking_now()) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        /* Let through the release of a key that was already down when the break
         * started, otherwise it would stay "stuck" after the break. */
        if ((wp == WM_KEYUP || wp == WM_SYSKEYUP) && (GetAsyncKeyState((int)k->vkCode) & 0x8000))
            return CallNextHookEx(NULL, code, wp, lp);
        return 1;
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && blocking_now()) {
        int vk = 0;
        switch (wp) {
        case WM_LBUTTONUP: vk = VK_LBUTTON; break;
        case WM_RBUTTONUP: vk = VK_RBUTTON; break;
        case WM_MBUTTONUP: vk = VK_MBUTTON; break;
        case WM_XBUTTONUP:
            vk = HIWORD(((const MSLLHOOKSTRUCT *)lp)->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            break;
        }
        if (vk && (GetAsyncKeyState(vk) & 0x8000))
            return CallNextHookEx(NULL, code, wp, lp);
        return 1;
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

#define BLOCK_GRACE_MS 10000 /* hooks expire this long after the break should have ended */

/* Hooks live on their own thread that does nothing else, so they always answer
 * instantly and Windows never times them out, whatever the UI thread is doing.
 * The thread also ends by itself once the deadline has passed, and then undoes
 * the Task Manager lock: even a frozen UI thread cannot keep the PC locked. */
static DWORD WINAPI hook_thread(LPVOID ready)
{
    MSG m;
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE); /* create the queue */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_hook, g_inst, 0);
    DWORD ek = hk ? 0 : GetLastError();
    HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, g_inst, 0);
    DWORD em = hm ? 0 : GetLastError();
    if (ready) SetEvent((HANDLE)ready);
    if (!hk) log_error(L"Could not block the keyboard (error %lu)", ek);
    if (!hm) log_error(L"Could not block the mouse (error %lu)", em);
    int quit = 0;
    while (!quit) {
        ULONGLONG now = GetTickCount64(), end = (ULONGLONG)g_blockUntil + 1000;
        if (now >= end) {          /* watchdog: the UI thread did not stop us in time */
            if (g_blockUntil)
                log_warn(L"Watchdog: the break should have ended over 10 s ago, input released by the safety net");
            taskmgr_unlock();
            break;
        }
        ULONGLONG left = end - now;
        MsgWaitForMultipleObjects(0, NULL, FALSE, left > 1000 ? 1000 : (DWORD)left, QS_ALLINPUT);
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
            if (m.message == WM_QUIT) quit = 1;
    }
    if (hk) UnhookWindowsHookEx(hk);
    if (hm) UnhookWindowsHookEx(hm);
    return 0;
}

static void hooks_start(void)
{
    if (g_hookThread) return;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hookThread = CreateThread(NULL, 64 * 1024, hook_thread, ready, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_hookThreadId);
    if (g_hookThread && ready) WaitForSingleObject(ready, 2000);
    if (ready) CloseHandle(ready);
}

static void hooks_stop(void)
{
    if (!g_hookThread) return;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hookThread, 2000);
    CloseHandle(g_hookThread);
    g_hookThread = NULL;
}

/* ------------------------------------------------------------- utilities */

typedef struct { ULONG Version, ControlMask, StateMask; } PowerThrottling;

/* EcoQoS between breaks (Windows 11 "efficiency mode"), full speed during breaks. */
static void set_eco(BOOL on)
{
    PowerThrottling s = { 1, 1 /* EXECUTION_SPEED */, on ? 1u : 0u };
    SetProcessInformation(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)4 /* ProcessPowerThrottling */, &s, sizeof s);
}

static void trim_memory(void)
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void fmt_clock(WCHAR *b, ULONGLONG secs)
{
    unsigned s = (unsigned)secs;
    if (s >= 3600) wsprintfW(b, L"%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else           wsprintfW(b, L"%u:%02u", s / 60, s % 60);
}

static ULONGLONG secs_left(ULONGLONG at, ULONGLONG now) { return at > now ? (at - now + 999) / 1000 : 0; }

/* ms until a ceil-rounded seconds countdown to `at` shows a new value */
static UINT ms_to_next_second(ULONGLONG at, ULONGLONG now)
{
    if (at <= now) return 1;
    ULONGLONG rem = at - now, secs = (rem + 999) / 1000;
    return (UINT)(rem - (secs - 1) * 1000) + 1;
}

/* Which break comes next, and when. Returns 0 when no break type is enabled. */
static int next_break(int *type, ULONGLONG *at)
{
    ULONGLONG miniAt = g_miniRef + (ULONGLONG)g_cfg.miniEveryMin * 60000;
    ULONGLONG longAt = g_longRef + (ULONGLONG)g_cfg.longEveryMin * 60000;
    /* A mini break due shortly before a long break is skipped. */
    if (g_cfg.longEnabled && (!g_cfg.miniEnabled || longAt <= miniAt + (ULONGLONG)g_cfg.miniEveryMin * 30000)) {
        *type = BT_LONG; *at = longAt; return 1;
    }
    if (g_cfg.miniEnabled) { *type = BT_MINI; *at = miniAt; return 1; }
    return 0;
}

static void fmt_status(WCHAR *b, BOOL forDialog)
{
    if (g_state == ST_BREAK) { lstrcpyW(b, T(g_breakType == BT_LONG ? S_BREAK_RUNNING : S_MINI_RUNNING)); return; }
    if (g_pauseFlags & PAUSE_AWAY) { lstrcpyW(b, T(S_PAUSED)); return; }
    if (g_pauseFlags)        { lstrcpyW(b, T(S_PAUSED_IDLE)); return; }
    if (g_gamingNow)         { wsprintfW(b, T(S_PAUSED_FOR), g_pauseProc); return; }
    int type; ULONGLONG at;
    if (!next_break(&type, &at)) { lstrcpyW(b, T(S_NONE)); return; }
    ULONGLONG s = secs_left(at, now_ms());
    const WCHAR *name = T(type == BT_LONG ? S_BREAK : S_MINI_BREAK);
    if (forDialog) {
        WCHAR c[24];
        fmt_clock(c, s);
        wsprintfW(b, T(S_NEXT_FMT), T(type == BT_LONG ? S_BREAK_LC : S_MINI_BREAK_LC), c);
    } else if (s >= 60) {
        wsprintfW(b, T(S_IN_MIN_FMT), name, (unsigned)((s + 59) / 60));
    } else {
        wsprintfW(b, T(S_IN_SEC_FMT), name, (unsigned)s);
    }
}

static HFONT make_font(int px, int weight, const WCHAR *face, DWORD quality)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, quality, DEFAULT_PITCH | FF_SWISS, face);
}

/* Best effort: become the foreground window. Deliberately no AttachThreadInput
 * or other trick that would tie us to another process's thread (anti-cheat safety);
 * the input hooks block keyboard and mouse anyway. */
static void force_foreground(HWND h)
{
    if (GetForegroundWindow() == h) return;
    BringWindowToTop(h);
    SetForegroundWindow(h);
}

/* ----------------------------------------------------------- break screen */

static void overlay_layout(Overlay *o, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (o->fTitle) DeleteObject(o->fTitle);
    if (o->fTime)  DeleteObject(o->fTime);
    /* Sizes follow the monitor's pixel size, so any resolution/orientation works. */
    int m = w < h ? w : h;
    o->fTitle = make_font(m * 5 / 100, FW_NORMAL, L"Segoe UI", CLEARTYPE_QUALITY);
    o->fTime  = make_font(m * 24 / 100, FW_LIGHT, L"Segoe UI Light", ANTIALIASED_QUALITY);

    HDC dc = GetDC(o->hwnd);
    TEXTMETRICW tm;
    HGDIOBJ old = SelectObject(dc, o->fTitle);
    GetTextMetricsW(dc, &tm);
    int th = tm.tmHeight;
    SelectObject(dc, o->fTime);
    GetTextMetricsW(dc, &tm);
    int ch = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(o->hwnd, dc);

    int gap = m / 100, y = (h - (th + gap + ch)) * 45 / 100;
    SetRect(&o->rcTitle, 0, y, w, y + th);
    SetRect(&o->rcTime, 0, y + th + gap, w, y + th + gap + ch);
}

static void draw_centered(HDC dc, HFONT f, const RECT *r, const WCHAR *s, COLORREF color)
{
    SIZE sz;
    int n = lstrlenW(s);
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, color);
    GetTextExtentPoint32W(dc, s, n, &sz);
    /* ETO_OPAQUE paints background and text in one go: no flicker, no back buffer. */
    ExtTextOutW(dc, r->left + (r->right - r->left - sz.cx) / 2, r->top, ETO_OPAQUE | ETO_CLIPPED, r, s, (UINT)n, NULL);
    SelectObject(dc, old);
}

static void overlay_paint(const Overlay *o, HDC dc, const RECT *area)
{
    WCHAR t[24];
    fmt_clock(t, secs_left(g_breakEnd, now_ms()));
    int saved = SaveDC(dc);
    ExcludeClipRect(dc, o->rcTitle.left, o->rcTitle.top, o->rcTitle.right, o->rcTitle.bottom);
    ExcludeClipRect(dc, o->rcTime.left, o->rcTime.top, o->rcTime.right, o->rcTime.bottom);
    FillRect(dc, area, (HBRUSH)GetStockObject(BLACK_BRUSH));
    RestoreDC(dc, saved);
    SetBkColor(dc, RGB(0, 0, 0));
    draw_centered(dc, o->fTitle, &o->rcTitle, T(g_breakType == BT_LONG ? S_BREAK : S_MINI_BREAK), RGB(130, 130, 130));
    draw_centered(dc, o->fTime, &o->rcTime, t, RGB(235, 235, 235));
}

static LRESULT CALLBACK overlay_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Overlay *o = (Overlay *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        o = (Overlay *)((CREATESTRUCTW *)lp)->lpCreateParams;
        o->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)o);
        break;
    case WM_SIZE:
        if (o) { overlay_layout(o, LOWORD(lp), HIWORD(lp)); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        if (!o) break;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        overlay_paint(o, dc, &ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_CLOSE:
    case WM_SYSCOMMAND:
        return 0; /* the break cannot be dismissed */
    case WM_DPICHANGED:
        return 0; /* layout depends on pixel size only */
    case WM_DESTROY:
        if (o) {
            if (o->fTitle) DeleteObject(o->fTitle);
            if (o->fTime)  DeleteObject(o->fTime);
            o->fTitle = o->fTime = NULL;
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static BOOL CALLBACK enum_monitor(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    (void)dc; (void)rc; (void)lp;
    MONITORINFO mi = { sizeof mi };
    if (g_nov >= MAX_OVERLAYS || !GetMonitorInfoW(hm, &mi)) return TRUE;
    Overlay *o = &g_ov[g_nov];
    ZeroMemory(o, sizeof *o);
    RECT r = mi.rcMonitor;
    if (CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CLS_OVERLAY, APP_NAME, WS_POPUP,
                        r.left, r.top, r.right - r.left, r.bottom - r.top, NULL, NULL, g_inst, o))
        g_nov++;
    return TRUE;
}

static void overlays_create(void)
{
    g_nov = 0;
    EnumDisplayMonitors(NULL, NULL, enum_monitor, 0);
    for (int i = 0; i < g_nov; i++)
        SetWindowPos(g_ov[i].hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (g_show ? SWP_SHOWWINDOW : 0));
    if (g_nov && g_show) force_foreground(g_ov[0].hwnd);
    SetCursor(NULL);
}

static void overlays_destroy(void)
{
    for (int i = 0; i < g_nov; i++)
        if (g_ov[i].hwnd) DestroyWindow(g_ov[i].hwnd);
    g_nov = 0;
}

static int is_overlay(HWND h)
{
    for (int i = 0; i < g_nov; i++)
        if (g_ov[i].hwnd == h) return 1;
    return 0;
}

/* -------------------------------------------------------- warning popup */

static void warn_close(void)
{
    if (g_warn) { DestroyWindow(g_warn); g_warn = NULL; }
    if (g_warnFont) { DeleteObject(g_warnFont); g_warnFont = NULL; }
    if (g_state == ST_WARN) g_state = ST_IDLE;
}

static void warn_draw(HDC dc, RECT rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(28, 28, 28)), ac = CreateSolidBrush(RGB(15, 118, 110));
    FillRect(dc, &rc, bg);
    RECT bar = rc;
    bar.right = MulDiv(5, (int)g_warnDpi, 96);
    FillRect(dc, &bar, ac);
    DeleteObject(bg);
    DeleteObject(ac);

    WCHAR t[64], c[24];
    ULONGLONG s = secs_left(g_warnAt, now_ms());
    const WCHAR *name = T(g_warnType == BT_LONG ? S_BREAK : S_MINI_BREAK);
    if (s >= 60) { fmt_clock(c, s); wsprintfW(t, T(S_IN_CLOCK_FMT), name, c); }
    else         wsprintfW(t, T(S_IN_SEC_FMT), name, (unsigned)s);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    HGDIOBJ of = SelectObject(dc, g_warnFont);
    rc.left = bar.right;
    DrawTextW(dc, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

static LRESULT CALLBACK warn_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        warn_draw(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

#ifdef PAUSERICK_TEST_TASK
static int g_timingLog; /* --timing-log: how late each countdown shows its new second */
static int g_offscreen; /* --offscreen: Settings drawn off the screen (screenshots) */
static void timing_log(const WCHAR *what, ULONGLONG at)
{
    ULONGLONG now = now_ms();
    if (!g_timingLog || at <= now) return;
    ULONGLONG rem = at - now, shown = (rem + 999) / 1000;
    log_info(L"timing: %s shows %u s, updated %u ms after that second began", what, (unsigned)shown,
             (unsigned)(shown * 1000 - rem));
}
#endif

static void warn_show(int type, ULONGLONG at)
{
    g_warnType = type;
    g_warnAt = at;
#ifdef PAUSERICK_TEST_TASK
    timing_log(L"warning", at);
#endif
    if (g_warn) { InvalidateRect(g_warn, NULL, FALSE); return; }

    POINT origin = { 0, 0 };
    HMONITOR hm = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(hm, &mi);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    g_warnDpi = dx;
    int w = MulDiv(250, (int)dx, 96), hh = MulDiv(54, (int)dx, 96), mg = MulDiv(16, (int)dx, 96);
    g_warnFont = make_font(MulDiv(17, (int)dx, 96), FW_SEMIBOLD, L"Segoe UI", CLEARTYPE_QUALITY);
    /* A plain opaque window: no layered/transparent/click-through styles, which
     * are what overlay cheats use and what anti-cheats look for. */
    g_warn = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             CLS_WARN, APP_NAME, WS_POPUP,
                             mi.rcWork.right - w - mg, mi.rcWork.bottom - hh - mg, w, hh, NULL, NULL, g_inst, NULL);
    if (!g_warn) { log_error(L"Could not show the warning (error %lu)", GetLastError()); return; }
    if (g_show) ShowWindow(g_warn, SW_SHOWNOACTIVATE);
    g_state = ST_WARN;
    if (g_show) log_info(L"Warning shown: %s in %u s", type == BT_LONG ? L"break" : L"mini break",
                         (unsigned)secs_left(at, now_ms()));
}

/* ----------------------------------------------- gaming / watching detection */
/*
 * A fullscreen game and a fullscreen YouTube video look the same to Windows, so
 * gaming mode looks at *what* is in the foreground: the folder its exe lives in
 * (game libraries), a DirectX exclusive-fullscreen state, or the user's list.
 * Browsers and video players never match, so videos still get their breaks.
 * Results are cached per foreground window: normally one cheap check every 5 s.
 */

enum { GK_OTHER, GK_GAME };

static const WCHAR *const GAME_DIRS[] = {
    L"\\steamapps\\common\\", L"\\epic games\\", L"\\gog games\\", L"\\gog galaxy\\games\\",
    L"\\xboxgames\\", L"\\modifiablewindowsapps\\", L"\\riot games\\", L"\\ea games\\",
    L"\\origin games\\", L"\\ubisoft game launcher\\games\\", L"\\rockstar games\\",
    L"\\amazon games\\library\\", L"\\itch\\apps\\",
};
/* launchers and tools that live inside those folders */
static const WCHAR *const NOT_GAME_DIRS[] = {
    L"\\epic games\\launcher\\", L"\\riot games\\riot client\\", L"\\wallpaper_engine\\",
    L"\\steamworks shared\\",
};

static HWND  g_fgHwnd;
static DWORD g_fgPid;
static int   g_fgKind;

/* Next ';'-separated entry of a game list, trimmed (truncated to cch). 0 = no more. */
static int list_next(const WCHAR **p, WCHAR *tok, int cch)
{
    const WCHAR *s = *p;
    while (*s) {
        const WCHAR *e = s;
        while (*e && *e != L';') e++;
        const WCHAR *a = s, *b = e;
        while (a < b && (*a == L' ' || *a == L'\t')) a++;
        while (b > a && (b[-1] == L' ' || b[-1] == L'\t')) b--;
        s = *e ? e + 1 : e;
        if (b > a) {
            int n = (int)(b - a);
            lstrcpynW(tok, a, (n < cch ? n : cch - 1) + 1);
            *p = s;
            return 1;
        }
    }
    *p = s;
    return 0;
}

/* list entry "game" or "game.exe" vs. exe file name, ignoring case */
static int entry_is_name(const WCHAR *entry, const WCHAR *name)
{
    if (wcschr(entry, L'\\')) return 0;
    int n = lstrlenW(entry), m = lstrlenW(name);
    if (CompareStringOrdinal(entry, n, name, m, TRUE) == CSTR_EQUAL) return 1;
    return m == n + 4 && CompareStringOrdinal(entry, n, name, n, TRUE) == CSTR_EQUAL &&
           CompareStringOrdinal(name + n, 4, L".exe", 4, TRUE) == CSTR_EQUAL;
}

static int list_has_name(const WCHAR *list, const WCHAR *name)
{
    WCHAR tok[MAX_PATH];
    while (list_next(&list, tok, ARRAYSIZE(tok)))
        if (entry_is_name(tok, name)) return 1;
    return 0;
}

static int in_game_list(const WCHAR *path, const WCHAR *name) /* path is lowercase */
{
    const WCHAR *p = g_cfg.games;
    WCHAR tok[MAX_PATH];
    while (list_next(&p, tok, ARRAYSIZE(tok))) {
        if (wcschr(tok, L'\\')) {   /* a folder (or full path) */
            CharLowerW(tok);
            if (wcsstr(path, tok)) return 1;
        } else if (entry_is_name(tok, name)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Anti-cheat safety: Pauserick never opens a handle to another process (no
 * OpenProcess, no memory access, no injection). The exe path of the foreground
 * app is asked from the kernel by process id (SystemProcessIdInformation, the
 * same query Task Manager uses for process names), which never touches the
 * game's process object, so there is nothing for an anti-cheat to see.
 */
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } NtUnicodeString;
typedef struct { HANDLE ProcessId; NtUnicodeString ImageName; } NtProcessIdInfo;
typedef LONG (NTAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);
#define SystemProcessIdInformation 88

static int process_image_path(DWORD pid, WCHAR *out, int cch)
{
    static NtQuerySystemInformationFn query;
    if (!query)
        query = (NtQuerySystemInformationFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                   "NtQuerySystemInformation");
    if (!query || !pid) return 0;
    WCHAR nt[MAX_PATH * 2];
    NtProcessIdInfo info = { (HANDLE)(ULONG_PTR)pid, { 0, (USHORT)(sizeof nt - sizeof(WCHAR)), nt } };
    if (query(SystemProcessIdInformation, &info, sizeof info, NULL) < 0) return 0;
    nt[info.ImageName.Length / sizeof(WCHAR)] = 0;

    /* \Device\HarddiskVolume3\Games\x.exe -> D:\Games\x.exe (for folder entries) */
    DWORD drives = GetLogicalDrives();
    WCHAR letter[3] = L"A:", dev[MAX_PATH];
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        letter[0] = (WCHAR)(L'A' + i);
        if (!QueryDosDeviceW(letter, dev, ARRAYSIZE(dev))) continue;
        int dl = lstrlenW(dev);
        if (dl > 0 && CompareStringOrdinal(nt, dl, dev, dl, TRUE) == CSTR_EQUAL && nt[dl] == L'\\') {
            lstrcpynW(out, letter, cch);
            lstrcpynW(out + 2, nt + dl, cch - 2);
            return 1;
        }
    }
    lstrcpynW(out, nt, cch); /* unmapped volume: the device path still matches game folders */
    return 1;
}

static WCHAR g_fgName[64];   /* exe name of the last classified foreground app */
static const WCHAR *g_fgWhy = L""; /* why it counts as a game (for the log) */

static int classify_window(HWND w)
{
    DWORD pid = 0;
    WCHAR path[MAX_PATH * 2];
    GetWindowThreadProcessId(w, &pid);
    g_fgName[0] = 0;
    if (!process_image_path(pid, path, ARRAYSIZE(path))) return GK_OTHER;
    const WCHAR *base = wcsrchr(path, L'\\');
    lstrcpynW(g_fgName, base ? base + 1 : path, ARRAYSIZE(g_fgName));
    CharLowerW(path);
    const WCHAR *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (in_game_list(path, name)) { g_fgWhy = L"in your list"; return GK_GAME; }
    for (int i = 0; i < (int)ARRAYSIZE(NOT_GAME_DIRS); i++)
        if (wcsstr(path, NOT_GAME_DIRS[i])) return GK_OTHER;
    for (int i = 0; i < (int)ARRAYSIZE(GAME_DIRS); i++)
        if (wcsstr(path, GAME_DIRS[i])) { g_fgWhy = L"installed in a game library"; return GK_GAME; }
    return GK_OTHER;
}

static int is_shell_window(HWND w)
{
    WCHAR c[32];
    if (!GetClassNameW(w, c, ARRAYSIZE(c))) return 0;
    return !lstrcmpW(c, L"Progman") || !lstrcmpW(c, L"WorkerW") || !lstrcmpW(c, L"Shell_TrayWnd");
}

/* the window fills its whole monitor (fullscreen game, video, presentation) */
static int covers_monitor(HWND w)
{
    RECT r;
    MONITORINFO mi = { sizeof mi };
    if (!w || is_shell_window(w) || !GetWindowRect(w, &r) ||
        !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi))
        return 0;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

static int window_is_game(HWND w)
{
    if (!w || is_shell_window(w) || is_overlay(w)) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (w != g_fgHwnd || pid != g_fgPid) {
        g_fgHwnd = w;
        g_fgPid = pid;
        g_fgKind = classify_window(w);
    }
    if (g_fgKind == GK_GAME) return 1;
    if (!covers_monitor(w)) return 0;
    QUERY_USER_NOTIFICATION_STATE q; /* asks the shell, not the game */
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN) {
        g_fgWhy = L"DirectX exclusive fullscreen";
        return 1;
    }
    return 0;
}

static const CLSID CLSID_MMDevEnum = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID   IID_MMDevEnum   = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID   IID_AudioMeter  = { 0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 } };

/* IAudioMeterInformation (only forward-declared by MinGW's endpointvolume.h) */
typedef struct AudioMeter AudioMeter;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AudioMeter *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(AudioMeter *);
    ULONG   (STDMETHODCALLTYPE *Release)(AudioMeter *);
    HRESULT (STDMETHODCALLTYPE *GetPeakValue)(AudioMeter *, float *);
} AudioMeterVtbl;
struct AudioMeter { const AudioMeterVtbl *lpVtbl; };

/* Is sound coming out of the default speakers right now? (peak meter, no recording)
 * Only asked while the user is idle; COM is started and stopped around each check
 * so the audio components don't stay loaded (~1 MB) the rest of the time. */
static int audio_playing(void)
{
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 0;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    AudioMeter *meter = NULL;
    float peak = 0.0f;
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDevEnum, NULL, CLSCTX_INPROC_SERVER, &IID_MMDevEnum, (void **)&en)) &&
        SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
        SUCCEEDED(IMMDevice_Activate(dev, &IID_AudioMeter, CLSCTX_INPROC_SERVER, NULL, (void **)&meter)))
        meter->lpVtbl->GetPeakValue(meter, &peak);
    if (meter) meter->lpVtbl->Release(meter);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return peak > 0.0005f;
}

#ifdef PAUSERICK_TEST_TASK
static int g_fakeWatching = -1; /* idle test: 0/1 replaces the real check below */
#endif

/* No input, but is the user watching something? (fullscreen window or sound playing) */
static int user_watching(HWND fg)
{
#ifdef PAUSERICK_TEST_TASK
    if (g_fakeWatching >= 0) return g_fakeWatching;
#endif
    return covers_monitor(fg) || audio_playing();
}

/* Windows that don't mean the user left the app they were in: Pauserick's own
 * (Settings, tray menu), the taskbar and its tray flyouts, or none (mid-switch).
 * Opening Settings from a game therefore still shows "paused for process X". */
static int is_passthrough_window(HWND w)
{
    DWORD pid = 0;
    WCHAR c[40];
    if (!w) return 1;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return 1;
    if (!GetClassNameW(w, c, ARRAYSIZE(c))) return 0;
    return !lstrcmpW(c, L"Shell_TrayWnd") || !lstrcmpW(c, L"Shell_SecondaryTrayWnd") ||
           !lstrcmpW(c, L"NotifyIconOverflowWindow") || !lstrcmpW(c, L"TopLevelWindowForOverflowXamlIsland");
}

/* ------------------------------------------------------------ pause media */
/*
 * When a break starts, whatever is playing (a video in the browser, Spotify, a
 * film app...) is paused through Windows' own media controls: the panel shown
 * with the volume, which the keyboard media keys also drive. Only the system
 * media service is asked: no message, handle or input goes to the players.
 * It is always "pause", never "play/pause", so nothing silent starts playing.
 * A short-lived thread does it, so the break screen never waits for it.
 */

typedef void (*RtFn)(void);
typedef struct { const RtFn *vt; } RtObj; /* any COM / WinRT interface pointer; methods by vtable slot */
typedef HRESULT (STDMETHODCALLTYPE *RtOutFn)(RtObj *, void *);        /* HRESULT Method([out] T *) */
typedef HRESULT (STDMETHODCALLTYPE *RtAtFn)(RtObj *, UINT32, void *); /* IVectorView<T>::GetAt */
typedef HRESULT (STDMETHODCALLTYPE *RtQiFn)(RtObj *, REFIID, void **);
typedef ULONG   (STDMETHODCALLTYPE *RtRelFn)(RtObj *);

/* vtable slots, as listed in the Windows.Media.Control metadata (0-5 are IInspectable) */
enum {
    SLOT_QI = 0, SLOT_RELEASE = 2,
    SLOT_STATICS_REQUEST = 6, /* GlobalSystemMediaTransportControlsSessionManagerStatics::RequestAsync */
    SLOT_MGR_SESSIONS    = 7, /* ...SessionManager::GetSessions */
    SLOT_SES_APP         = 6, /* ...Session::get_SourceAppUserModelId */
    SLOT_SES_PLAYBACK    = 9, /* ...Session::GetPlaybackInfo */
    SLOT_SES_PAUSE       = 11,/* ...Session::TryPauseAsync */
    SLOT_INFO_STATUS     = 7, /* ...SessionPlaybackInfo::get_PlaybackStatus */
    SLOT_VEC_AT = 6, SLOT_VEC_SIZE = 7, /* IVectorView<T> */
    SLOT_OP_RESULTS      = 8, /* IAsyncOperation<T>::GetResults */
    SLOT_ASYNC_STATUS    = 7, /* IAsyncInfo::get_Status */
};
enum { ASYNC_STARTED = 0, ASYNC_COMPLETED = 1 };
#define PLAYBACK_PLAYING 4

static const IID IID_SmtcManagerStatics = { 0x2050C4EE, 0x11A0, 0x57DE, { 0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45 } };
static const IID IID_RtAsyncInfo        = { 0x00000036, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static HRESULT rt_get(RtObj *o, int slot, void *out) { return ((RtOutFn)o->vt[slot])(o, out); }
static void rt_release(RtObj *o) { if (o) ((RtRelFn)o->vt[SLOT_RELEASE])(o); }

/* Waits up to ms for an IAsyncOperation<T> to finish, then fetches its result. */
static HRESULT rt_await(RtObj *op, DWORD ms, void *result)
{
    RtObj *info = NULL;
    int st = ASYNC_STARTED;
    HRESULT hr = ((RtQiFn)op->vt[SLOT_QI])(op, &IID_RtAsyncInfo, (void **)&info);
    if (FAILED(hr)) return hr;
    for (DWORD waited = 0; SUCCEEDED(rt_get(info, SLOT_ASYNC_STATUS, &st)) && st == ASYNC_STARTED && waited < ms; waited += 10)
        Sleep(10);
    rt_release(info);
    if (st == ASYNC_STARTED) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return st == ASYNC_COMPLETED ? rt_get(op, SLOT_OP_RESULTS, result) : E_FAIL;
}

/* Pauses media session i if it is playing; adds its app to `done` (for the log). */
static void media_pause_session(RtObj *list, UINT32 i, WCHAR *done, int cch)
{
    RtObj *ses = NULL, *info = NULL, *op = NULL;
    int status = 0;
    if (SUCCEEDED(((RtAtFn)list->vt[SLOT_VEC_AT])(list, i, &ses)) &&
        SUCCEEDED(rt_get(ses, SLOT_SES_PLAYBACK, &info)) &&
        SUCCEEDED(rt_get(info, SLOT_INFO_STATUS, &status)) && status == PLAYBACK_PLAYING) {
        HSTRING app = NULL;
        unsigned char ok = 0; /* WinRT boolean */
        const WCHAR *name = SUCCEEDED(rt_get(ses, SLOT_SES_APP, &app)) ? WindowsGetStringRawBuffer(app, NULL) : NULL;
        if (!name || !name[0]) name = L"?";
        HRESULT hr = rt_get(ses, SLOT_SES_PAUSE, &op);
        if (SUCCEEDED(hr)) hr = rt_await(op, 2000, &ok);
        if (SUCCEEDED(hr) && ok) {
            if (lstrlenW(done) + lstrlenW(name) + 3 < cch) {
                if (done[0]) lstrcatW(done, L", ");
                lstrcatW(done, name);
            }
        } else {
            log_warn(L"Could not pause the media playing in %s (0x%08lX)", name, (unsigned long)(FAILED(hr) ? hr : S_FALSE));
        }
        if (app) WindowsDeleteString(app);
    }
    rt_release(op);
    rt_release(info);
    rt_release(ses);
}

static DWORD WINAPI media_pause_thread(LPVOID unused)
{
    (void)unused;
    static const WCHAR cls[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) { log_warn(L"Could not pause media: WinRT not available (0x%08lX)", (unsigned long)hr); return 0; }
    HSTRING name = NULL;
    RtObj *statics = NULL, *op = NULL, *mgr = NULL, *list = NULL;
    UINT32 n = 0;
    WCHAR done[256] = L"";
    hr = WindowsCreateString(cls, ARRAYSIZE(cls) - 1, &name);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, &IID_SmtcManagerStatics, (void **)&statics);
    if (SUCCEEDED(hr)) hr = rt_get(statics, SLOT_STATICS_REQUEST, &op);
    if (SUCCEEDED(hr)) hr = rt_await(op, 3000, &mgr);
    if (SUCCEEDED(hr) && !mgr) hr = E_POINTER;
    if (SUCCEEDED(hr)) hr = rt_get(mgr, SLOT_MGR_SESSIONS, &list);
    if (SUCCEEDED(hr) && !list) hr = E_POINTER;
    if (SUCCEEDED(hr)) hr = rt_get(list, SLOT_VEC_SIZE, &n);
    for (UINT32 i = 0; SUCCEEDED(hr) && i < n && i < 32; i++) media_pause_session(list, i, done, ARRAYSIZE(done));
    if (FAILED(hr))   log_warn(L"Could not pause media: Windows media controls did not answer (0x%08lX)", (unsigned long)hr);
    else if (done[0]) log_info(L"Media paused for the break: %s", done);
    rt_release(list);
    rt_release(mgr);
    rt_release(op);
    rt_release(statics);
    if (name) WindowsDeleteString(name);
    RoUninitialize();
    return 0;
}

static void media_pause_start(void)
{
    HANDLE t = CreateThread(NULL, 0, media_pause_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else   log_warn(L"Could not pause media (error %lu)", GetLastError());
}

/* ------------------------------------------------------------- scheduler */

#define QUIET_CONFIRM_MS 30000 /* nothing playing for this long before an idle pause starts */

static HWND g_appHwnd; /* the app the user is really in (ignores our own windows and the taskbar) */

static void tick(void);
static void tray_show_paused(int paused);

/* tolerance 0 = on time. (For SetCoalescableTimer 0 would mean "the system default", which
 * lets Windows delay the timers of a hidden window such as ours: the warning's countdown
 * then lagged behind. A countdown must not, so 0 becomes "no coalescing".) */
static void set_tick(UINT ms, UINT tolerance)
{
    if (ms < 10) ms = 10;
    SetCoalescableTimer(g_main, TIMER_TICK, ms, NULL, tolerance ? tolerance : TIMERV_NO_COALESCING);
}

/* Undo everything a break set up (screens, input block, Task Manager lock). */
static void break_teardown(void)
{
    overlays_destroy();
    InterlockedExchange64(&g_blockUntil, 0); /* hooks stop blocking right away */
    hooks_stop();
    taskmgr_unlock();
    SetThreadExecutionState(ES_CONTINUOUS);
    g_state = ST_IDLE;
    ULONGLONG now = now_ms();
    g_miniRef = now;
    if (g_breakType == BT_LONG) g_longRef = now;
    g_activeSince = now; /* input swallowed during the break doesn't make the user idle */
    set_eco(TRUE);
    trim_memory();
}

static const WCHAR *break_name(int type) { return type == BT_LONG ? L"Break" : L"Mini break"; }

/* ---------------------------------------------------------------- pauses */
/*
 * The timers stop while the PC is locked or asleep, and while it is idle (setting).
 * When the last reason is gone they run again. With "restart" chosen they start from
 * zero after an idle pause, or after a lock / sleep at least as long as the idle time;
 * otherwise (and always with the idle pause off) they continue exactly where they stopped.
 */
static void pause_begin(int flag, ULONGLONG at)
{
    if (!g_pauseFlags) { g_pausedSince = at; g_pauseWasIdle = 0; }
    if (flag == PAUSE_IDLE) g_pauseWasIdle = 1;
    else if (!(g_pauseFlags & flag)) /* the idle pause writes its own line, with the reason */
        log_info(flag == PAUSE_LOCK ? L"PC locked: timers paused" : L"PC going to sleep: timers paused");
    g_pauseFlags |= flag;
    if (g_state == ST_WARN) warn_close();
}

/* The reasons in mask are over as of `at`; if none is left, the timers run again. */
static void pause_clear(int mask, ULONGLONG at)
{
    if (!(g_pauseFlags & mask)) return;
    if (mask & PAUSE_AWAY) g_activeSince = at;
    g_pauseFlags &= ~mask;
    if (g_pauseFlags) return;
    ULONGLONG away = at > g_pausedSince ? at - g_pausedSince : 0;
    int restart = g_cfg.idlePause && !g_cfg.idleContinue &&
                  (g_pauseWasIdle || away >= (ULONGLONG)g_cfg.idlePauseMin * 60000);
    WCHAR left[48] = L"";
    if (restart) {
        g_miniRef = g_longRef = at;
    } else {
        g_miniRef += away;
        g_longRef += away;
        int type;
        ULONGLONG nb;
        if (next_break(&type, &nb))
            wsprintfW(left, L" (%s in %u min)", type == BT_LONG ? L"break" : L"mini break",
                      (unsigned)((secs_left(nb, at) + 59) / 60));
    }
    log_info(L"%s after %u min paused: timers %s%s",
             (mask & PAUSE_LOCK) ? L"PC unlocked" : (mask & PAUSE_SUSPEND) ? L"PC resumed from sleep" : L"Back at the PC",
             (unsigned)(away / 60000), restart ? L"restart from zero" : L"continue where they stopped", left);
}

static void pause_end(int mask)
{
    if (!(g_pauseFlags & mask)) return;
    pause_clear(mask, now_ms());
    if (g_state != ST_BREAK) tick();
}

/*
 * Idle pause: no keyboard or mouse input for idlePauseMin and nothing playing (no
 * sound, no fullscreen window) for the last 30 s, so a video still gets its breaks.
 * The sound checks start 30 s before the limit, so the pause starts on time.
 * Any input ends the pause. Returns 1 while Pauserick is paused for this reason.
 */
static int idle_paused(HWND fg, ULONGLONG now, ULONGLONG lastInput)
{
    static ULONGLONG quietSince; /* nothing playing since then (0 = not seen yet) */
    static int checked, loggedWatching;
    if (g_pauseFlags & PAUSE_IDLE) {
        if (lastInput <= g_pausedSince && g_cfg.idlePause) return 1;
        pause_clear(PAUSE_IDLE, lastInput > g_pausedSince ? lastInput : now); /* back since the first input */
        return 0;
    }
    if (lastInput < g_activeSince) lastInput = g_activeSince;
    ULONGLONG idle = now > lastInput ? now - lastInput : 0, limit = (ULONGLONG)g_cfg.idlePauseMin * 60000;
    if (!g_cfg.idlePause || idle + QUIET_CONFIRM_MS < limit) {
        quietSince = 0;
        loggedWatching = 0;
        if (checked) { checked = 0; trim_memory(); } /* drop what the sound checks paged in */
        return 0;
    }
    checked = 1;
    if (user_watching(fg)) {
        quietSince = 0;
        if (!loggedWatching && idle >= limit) {
            log_info(L"No input for %u min, but sound or a fullscreen window: treated as watching, breaks continue",
                     (unsigned)(idle / 60000));
            loggedWatching = 1;
        }
        return 0;
    }
    if (!quietSince) quietSince = now;
    if (idle < limit || now - quietSince < QUIET_CONFIRM_MS) return 0;
    pause_begin(PAUSE_IDLE, now);
    log_info(L"Idle: no input for %u min and nothing playing: Pauserick paused (when back: %s)",
             (unsigned)(idle / 60000), resume_name(g_cfg.idleContinue));
    quietSince = 0;
    loggedWatching = checked = 0;
    trim_memory();
    return 1;
}

/* "2560x1440, 1920x1080, ..." for the log */
static void describe_screens(WCHAR *out, int cch)
{
    out[0] = 0;
    for (int i = 0; i < g_nov; i++) {
        RECT r;
        WCHAR one[40];
        if (!GetWindowRect(g_ov[i].hwnd, &r)) continue;
        wsprintfW(one, L"%s%ldx%ld", out[0] ? L", " : L"", r.right - r.left, r.bottom - r.top);
        if (lstrlenW(out) + lstrlenW(one) < cch) lstrcatW(out, one);
    }
}

static void start_break(int type, const WCHAR *why)
{
    if (g_state == ST_BREAK) return;
    EndMenu();
    warn_close();
    pause_clear(PAUSE_IDLE, now_ms()); /* asked for from the tray or the command line: someone is here */
    g_state = ST_BREAK;
    g_breakType = type;
    g_breakEnd = now_ms() + (type == BT_LONG ? (ULONGLONG)g_cfg.longDurMin * 60000 : (ULONGLONG)g_cfg.miniDurSec * 1000);
    g_lastShownSecs = -1;
    set_eco(FALSE);
    InterlockedExchange64(&g_blockUntil, (LONGLONG)(g_breakEnd + BLOCK_GRACE_MS));
#ifdef PAUSERICK_TEST_TASK
    if (!g_show) InterlockedExchange64(&g_blockUntil, 0); /* test "quiet break": nothing blocked */
#endif
    hooks_start(); /* block input first, then cover the screens */
    if (g_elevated && strict_registered()) taskmgr_lock(); /* strict mode only: needs admin rights */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    overlays_create();
    if (!g_nov) { /* no screen to show the break on: never block input blind */
        log_warn(L"%s (%s) cancelled: no screen to show it on, input was not blocked", break_name(type), why);
        break_teardown();
        tick();
        return;
    }
    if (g_show) media_pause_start(); /* the video or music playing behind the break screen stops */
    WCHAR screens[200];
    describe_screens(screens, ARRAYSIZE(screens));
    log_info(L"%s started (%s): %u s on %d screen(s) [%s], keyboard and mouse blocked", break_name(type), why,
             (unsigned)secs_left(g_breakEnd, now_ms()), g_nov, screens);
    tick();
}

static void end_break(void)
{
    log_info(L"%s ended", break_name(g_breakType));
    break_teardown();
    if (g_cfg.sound) MessageBeep(MB_ICONASTERISK);
    tick();
}

static void tick(void)
{
    ULONGLONG now = now_ms();

    if (g_state == ST_BREAK) {
        if (now >= g_breakEnd) { end_break(); return; }
        LONGLONG s = (LONGLONG)secs_left(g_breakEnd, now);
        int fgOk = is_overlay(GetForegroundWindow());
        for (int i = 0; i < g_nov; i++) {
            if (s != g_lastShownSecs) InvalidateRect(g_ov[i].hwnd, &g_ov[i].rcTime, FALSE);
            /* stay above anything that pops up */
            SetWindowPos(g_ov[i].hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        g_lastShownSecs = s;
        if (!fgOk && g_nov) force_foreground(g_ov[0].hwnd);
        UINT d = ms_to_next_second(g_breakEnd, now);
        set_tick(d > 500 ? 500 : d, 0);
        return;
    }

    if (g_pauseFlags & PAUSE_AWAY) { warn_close(); set_tick(IDLE_TICK_MS, 1000); return; }
    if (g_updRestart) { g_updRestart = 0; update_restart(); return; } /* updated during the break that just ended */

    HWND fg = GetForegroundWindow();

    /* Gaming mode: the app in front is a game (recognized, or in the user's list).
     * The timers start fresh afterwards, so a quick Alt-Tab doesn't trigger an overdue break.
     * Settings, the tray menu and the taskbar don't count as leaving the game. */
    if (!is_passthrough_window(fg))             g_appHwnd = fg;
    else if (g_appHwnd && !IsWindow(g_appHwnd)) g_appHwnd = NULL; /* that app was closed meanwhile */
    g_gamingNow = g_cfg.gaming && window_is_game(g_appHwnd);
    if (g_gamingNow) lstrcpynW(g_pauseProc, g_fgName[0] ? g_fgName : L"?", ARRAYSIZE(g_pauseProc));
    {
        static int was;
        static WCHAR wasName[64];
        if (g_gamingNow && (!was || lstrcmpiW(wasName, g_pauseProc))) {
            log_info(L"Gaming mode: breaks paused for process %s (in front, %s)", g_pauseProc, g_fgWhy);
            lstrcpynW(wasName, g_pauseProc, ARRAYSIZE(wasName));
        } else if (!g_gamingNow && was) {
            log_info(L"Gaming mode: %s no longer detected, breaks resume with fresh timers", wasName);
        }
        was = g_gamingNow;
    }
    if (g_gamingNow) {
        g_pauseFlags &= ~PAUSE_IDLE; /* a game came to the front by itself: gaming mode takes over */
        tray_show_paused(1);         /* red pause bars while the breaks are on hold */
        g_miniRef = g_longRef = now;
        warn_close();
        set_tick(IDLE_TICK_MS, 1000);
        return;
    }

    /* Nobody at the PC (no input, nothing playing): pause until the next input. */
    LASTINPUTINFO li = { sizeof li, 0 };
    ULONGLONG lastInput = now;
    if (GetLastInputInfo(&li)) {
        DWORD ago = GetTickCount() - li.dwTime;
        lastInput = ago < now ? now - ago : 0;
    }
    int idle = idle_paused(fg, now, lastInput);
    tray_show_paused(idle);
    if (idle) {
        warn_close();
        set_tick(IDLE_TICK_MS, 1000);
        return;
    }

    int type;
    ULONGLONG at;
    int haveBreak = next_break(&type, &at);
    /* A new version is offered only at a quiet moment: no break or warning soon, no game,
     * no fullscreen window (video, presentation) in front, someone at the PC. */
    if (g_updPrompt && g_state == ST_IDLE && !covers_monitor(fg) && (!haveBreak || at > now + 120000)) {
        g_updPrompt = 0;
        PostMessageW(g_main, WM_APP_UPDATE_PROMPT, 0, 0);
    }
    if (!haveBreak) { warn_close(); set_tick(30000, 5000); return; }
    if (now >= at) { start_break(type, L"scheduled"); return; }

    ULONGLONG until = at - now, warnMs = (ULONGLONG)g_cfg.warnSec * 1000;
    if (warnMs && until <= warnMs) {
        warn_show(type, at);
        set_tick(ms_to_next_second(at, now), 0);
        return;
    }
    warn_close();
    until -= warnMs;
    if (until < IDLE_TICK_MS) set_tick((UINT)until, 0);
    else                      set_tick(IDLE_TICK_MS, 1000);
}

/* ---------------------------------------------------------------- updates */
/*
 * At most once a day (the first time 90 s after start, if a day has passed) one HTTPS
 * request asks GitHub for the latest release; nothing else in Pauserick ever goes
 * online, and it can be turned off. A newer version is offered in a dialog: install
 * now, not now (asked again the next day), or skip this version.
 * Installing downloads Pauserick.exe and Pauserick.exe.sig from that release and checks
 * the ECDSA P-256 signature, over the exe and the version it claims, against the public
 * key built into this exe (src/update_key.h). Only then are the files swapped and
 * Pauserick restarted. Without the maintainer's private key nobody, not even someone in
 * control of the GitHub repository, can get a file installed this way, nor pass off an
 * older signed version as a newer one.
 */
#ifdef UPDATE_KEY_FILE
#include UPDATE_KEY_FILE /* test build: a throwaway key */
#else
#include "update_key.h"
#endif

#define UPDATE_API_URL   L"https://api.github.com/repos/" PAUSERICK_REPO L"/releases/latest"
#define UPDATE_PAGE_URL  L"https://github.com/" PAUSERICK_REPO L"/releases/latest"
#define PROJECT_URL      L"https://github.com/" PAUSERICK_REPO
#define UPDATE_DAY       (24 * 3600)
#define UPDATE_RETRY     (3 * 3600)    /* after a failed check (offline...) */
#define UPDATE_FIRST_MS  90000u        /* first look after start */
#define UPDATE_TIMER_MS  3600000u      /* then hourly: is a day over? */
#define UPDATE_MAX_EXE   (8u << 20)

enum { UPD_OK = 0, UPD_UP_TO_DATE, UPD_ERR_NET, UPD_ERR_DATA, UPD_ERR_SIG, UPD_ERR_FILE, UPD_ERR_RIGHTS, UPD_NONE = -1 };

typedef struct {
    int manual, result, web;       /* web: found through the website, GitHub's API was busy */
    DWORD err;
    WCHAR version[24];             /* "1.0.0" */
    WCHAR exeUrl[512], sigUrl[512];
    WCHAR notes[700];              /* release notes, plain text */
} UpdInfo;

static UpdInfo g_upd;               /* the newer version found (version[0] == 0: none) */
static volatile LONG g_updBusy;     /* a check or an install is running */
static int g_updLast = UPD_NONE;    /* result of the last check (for the Settings link) */

static LONGLONG unix_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (LONGLONG)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static LONGLONG ini_ll(const WCHAR *sec, const WCHAR *key)
{
    WCHAR b[32];
    LONGLONG v = 0;
    GetPrivateProfileStringW(sec, key, L"0", b, ARRAYSIZE(b), g_ini);
    for (const WCHAR *p = b; *p >= L'0' && *p <= L'9' && v < 1000000000000LL; p++) v = v * 10 + (*p - L'0');
    return v;
}

static void ini_put_ll(const WCHAR *sec, const WCHAR *key, LONGLONG v)
{
    WCHAR b[32], *p = b + 31;
    *p = 0;
    do { *--p = (WCHAR)(L'0' + v % 10); v /= 10; } while (v > 0 && p > b);
    WritePrivateProfileStringW(sec, key, p, g_ini);
}

/* "1.0.0" or "v1.0.0" -> 3 numbers; anything else (e.g. "1.0.0-beta") is refused */
static int ver_parse(const WCHAR *s, int v[3])
{
    if (*s == L'v' || *s == L'V') s++;
    for (int i = 0; i < 3; i++) {
        if (*s < L'0' || *s > L'9') return 0;
        for (v[i] = 0; *s >= L'0' && *s <= L'9'; s++)
            if ((v[i] = v[i] * 10 + (*s - L'0')) > 99999) return 0;
        if (i < 2 && *s++ != L'.') return 0;
    }
    return *s == 0;
}

static int ver_cmp3(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

static int ver_newer(const WCHAR *v)
{
    int a[3], b[3];
    return ver_parse(v, a) && ver_parse(APP_VERSION, b) && ver_cmp3(a, b) > 0;
}

/* An exe's version, from its version resource. */
static int file_version(const WCHAR *path, int v[3])
{
    DWORD dummy, size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size || size > 64 * 1024) return 0;
    void *buf = HeapAlloc(GetProcessHeap(), 0, size);
    VS_FIXEDFILEINFO *fi = NULL;
    UINT len = 0;
    int ok = buf && GetFileVersionInfoW(path, 0, size, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &len) &&
             fi && len >= sizeof *fi;
    if (ok) {
        v[0] = HIWORD(fi->dwFileVersionMS);
        v[1] = LOWORD(fi->dwFileVersionMS);
        v[2] = HIWORD(fi->dwFileVersionLS);
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

/* GET over HTTPS (plain HTTP only to this PC, in the test build). Returns the body,
 * NUL-terminated, at most max bytes, in a HeapAlloc'd buffer; NULL with *err set
 * (a Windows error code, or the HTTP status). Redirects are followed by WinHTTP, except
 * with location: then the redirect is not followed, its target is copied there and no
 * body is read. */
static char *http_request(const WCHAR *url, DWORD max, DWORD *len, DWORD *err, WCHAR *location, DWORD cchLocation)
{
    WCHAR host[256], path[2048];
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    *len = 0;
    *err = 0;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { *err = GetLastError(); return NULL; }
    int secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
#ifdef PAUSERICK_TEST_TASK
    if (!secure && lstrcmpiW(host, L"127.0.0.1")) { *err = ERROR_ACCESS_DENIED; return NULL; }
#else
    if (!secure) { *err = ERROR_ACCESS_DENIED; return NULL; }
#endif
    HINTERNET s = WinHttpOpen(L"Pauserick/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) /* before Windows 8.1 */
        s = WinHttpOpen(L"Pauserick/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = NULL, r = NULL;
    char *buf = NULL;
    DWORD status = 0, sz = sizeof status, cap = 0;
    if (s) {
        WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
        c = WinHttpConnect(s, host, uc.nPort, 0);
    }
    if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  secure ? WINHTTP_FLAG_SECURE : 0);
    if (r && location) {
        DWORD off = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        location[0] = 0;
    }
    if (r && WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json, */*", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD) &&
        WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
        if (location) { /* only where it redirects to */
            DWORD lb = cchLocation * sizeof(WCHAR);
            if (status < 300 || status >= 400) *err = status;
            else if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location, &lb,
                                          WINHTTP_NO_HEADER_INDEX)) {
                *err = GetLastError();
                location[0] = 0;
            }
        } else if (status != 200) {
            *err = status;
        }
        while (!*err && !location) {
            DWORD avail = 0, got = 0;
            if (!WinHttpQueryDataAvailable(r, &avail)) { *err = GetLastError(); break; }
            if (!avail) break;
            if (avail > max - *len) { *err = ERROR_FILE_TOO_LARGE; break; }
            DWORD need = *len + avail + 1;
            if (need > cap) {
                cap = need * 2 > max + 1 ? (need > max + 1 ? need : max + 1) : need * 2;
                char *nb = buf ? (char *)HeapReAlloc(GetProcessHeap(), 0, buf, cap) : (char *)HeapAlloc(GetProcessHeap(), 0, cap);
                if (!nb) { *err = ERROR_OUTOFMEMORY; break; }
                buf = nb;
            }
            if (!WinHttpReadData(r, buf + *len, avail, &got)) { *err = GetLastError(); break; }
            *len += got;
        }
    } else {
        *err = GetLastError();
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (location) {
        if (!*err && !location[0]) *err = ERROR_NO_DATA;
        return NULL;
    }
    if (*err || !buf) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        if (!*err) *err = ERROR_NO_DATA;
        return NULL;
    }
    buf[*len] = 0;
    return buf;
}

static char *http_get(const WCHAR *url, DWORD max, DWORD *len, DWORD *err)
{
    return http_request(url, max, len, err, NULL, 0);
}

/* Where url redirects to (not followed); 0 if it doesn't redirect (*err: the status). */
static int http_location(const WCHAR *url, WCHAR *out, DWORD cch, DWORD *err)
{
    DWORD len = 0;
    http_request(url, 0, &len, err, out, cch);
    return !*err && out[0];
}

static void utf8_put(char *out, int *n, int cap, unsigned u)
{
    char b[3];
    int k = u < 0x80 ? 1 : u < 0x800 ? 2 : 3;
    if (k == 1) b[0] = (char)u;
    else if (k == 2) { b[0] = (char)(0xC0 | (u >> 6)); b[1] = (char)(0x80 | (u & 0x3F)); }
    else { b[0] = (char)(0xE0 | (u >> 12)); b[1] = (char)(0x80 | ((u >> 6) & 0x3F)); b[2] = (char)(0x80 | (u & 0x3F)); }
    for (int i = 0; i < k && *n < cap - 1; i++) out[(*n)++] = b[i];
}

static int hex_val(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The string value of the first "key" at or after p, unescaped into out (UTF-8). Returns
 * a pointer past the value, or NULL. Enough for GitHub's release JSON, not a JSON parser. */
static const char *json_string(const char *p, const char *key, char *out, int cap)
{
    char pat[40];
    int kl = wsprintfA(pat, "\"%s\"", key);
    out[0] = 0;
    for (const char *k = strstr(p, pat); k; k = strstr(k + 1, pat)) {
        const char *s = k + kl;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (*s != ':') continue; /* the same text as a value, not as a key */
        for (s++; *s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'; s++) {}
        if (*s != '"') return s; /* null, a number... : empty */
        int n = 0;
        for (s++; *s && *s != '"'; ) {
            char c = *s++;
            if (c != '\\') { if (n < cap - 1) out[n++] = c; continue; }
            c = *s ? *s++ : 0;
            if (c == 'u') {
                unsigned u = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hex_val(*s);
                    if (h < 0) break;
                    u = u * 16 + (unsigned)h;
                    s++;
                }
                utf8_put(out, &n, cap, u >= 0xD800 && u <= 0xDFFF ? '?' : u);
            } else if (c) {
                char e = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' || c == 'f' ? ' ' : c;
                if (n < cap - 1) out[n++] = e;
            }
        }
        out[n] = 0;
        return *s ? s + 1 : s;
    }
    return NULL;
}

/* What the latest release offers: newer than us, with both signed files? */
static void update_parse(const char *json, UpdInfo *u)
{
    char tag[64], url[512];
    if (!json_string(json, "tag_name", tag, sizeof tag) || !tag[0]) { u->result = UPD_ERR_DATA; return; }
    WCHAR v[64];
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, v, ARRAYSIZE(v));
    int parts[3];
    if (!ver_parse(v, parts)) { u->result = UPD_ERR_DATA; return; }
    wsprintfW(u->version, L"%d.%d.%d", parts[0], parts[1], parts[2]);
    if (!ver_newer(u->version)) { u->result = UPD_UP_TO_DATE; return; }
    for (const char *p = json; (p = json_string(p, "browser_download_url", url, sizeof url)) != NULL; ) {
        size_t n = strlen(url);
        WCHAR *dst = n > 14 && !strcmp(url + n - 14, "/Pauserick.exe") ? u->exeUrl
                   : n > 18 && !strcmp(url + n - 18, "/Pauserick.exe.sig") ? u->sigUrl : NULL;
        if (dst) MultiByteToWideChar(CP_UTF8, 0, url, -1, dst, 512);
    }
    if (!u->exeUrl[0] || !u->sigUrl[0]) { u->result = UPD_ERR_DATA; return; } /* no signed exe in it */
    char *body = (char *)HeapAlloc(GetProcessHeap(), 0, 4096);
    WCHAR *wide = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, 4096 * sizeof(WCHAR));
    if (body && wide) { /* release notes, cut to what fits in the dialog */
        json_string(json, "body", body, 4096);
        if (!MultiByteToWideChar(CP_UTF8, 0, body, -1, wide, 4096)) wide[0] = 0;
        lstrcpynW(u->notes, wide, ARRAYSIZE(u->notes));
        if (lstrlenW(wide) >= (int)ARRAYSIZE(u->notes)) lstrcpyW(u->notes + ARRAYSIZE(u->notes) - 4, L"...");
    }
    if (body) HeapFree(GetProcessHeap(), 0, body);
    if (wide) HeapFree(GetProcessHeap(), 0, wide);
    u->result = UPD_OK;
}

static void update_api_url(WCHAR *url, int cch)
{
    lstrcpynW(url, UPDATE_API_URL, cch);
#ifdef PAUSERICK_TEST_TASK
    GetPrivateProfileStringW(L"Update", L"TestApi", url, url, (DWORD)cch, g_ini); /* local test server */
#endif
}

/* https://github.com/<repo>: its /releases/latest and /releases/download/<tag>/<file> */
static void update_web_base(WCHAR *url, int cch)
{
    lstrcpynW(url, L"https://github.com/" PAUSERICK_REPO, cch);
#ifdef PAUSERICK_TEST_TASK
    GetPrivateProfileStringW(L"Update", L"TestWeb", url, url, (DWORD)cch, g_ini);
#endif
}

/* Opens a web page in the default browser. Elevated (Block Task Manager), the address goes
 * through Explorer, which hands it to the desktop's unelevated Explorer: the browser must not
 * get administrator rights. Only https links: release notes can hold links too. */
static void open_url(const WCHAR *url)
{
    if (CompareStringOrdinal(url, 8, L"https://", 8, TRUE) != CSTR_EQUAL) return;
    WCHAR ex[MAX_PATH];
    UINT n = g_elevated ? GetWindowsDirectoryW(ex, MAX_PATH - 16) : 0;
    if (n && n < MAX_PATH - 16) {
        lstrcatW(ex, L"\\explorer.exe");
        if ((INT_PTR)ShellExecuteW(NULL, L"open", ex, url, NULL, SW_SHOWNORMAL) > 32) return;
    }
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

/* Does the signature (64 bytes, r||s) fit "PAUSERICK-UPDATE\n" + version + "\n" + exe? */
static int update_verify(const BYTE *exe, DWORD len, const char *version, const BYTE sig[64])
{
    static const char prefix[] = "PAUSERICK-UPDATE\n";
    BCRYPT_ALG_HANDLE sha = NULL, ecdsa = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    BYTE digest[32];
    int ok = BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
             BCryptCreateHash(sha, &h, NULL, 0, NULL, 0, 0) >= 0 &&
             BCryptHashData(h, (PUCHAR)prefix, sizeof prefix - 1, 0) >= 0 &&
             BCryptHashData(h, (PUCHAR)version, (ULONG)strlen(version), 0) >= 0 &&
             BCryptHashData(h, (PUCHAR)"\n", 1, 0) >= 0 &&
             BCryptHashData(h, (PUCHAR)exe, len, 0) >= 0 &&
             BCryptFinishHash(h, digest, sizeof digest, 0) >= 0 &&
             BCryptOpenAlgorithmProvider(&ecdsa, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) >= 0 &&
             BCryptImportKeyPair(ecdsa, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)UPDATE_PUBKEY, sizeof UPDATE_PUBKEY, 0) >= 0 &&
             BCryptVerifySignature(key, NULL, digest, sizeof digest, (PUCHAR)sig, 64, 0) >= 0;
    if (key) BCryptDestroyKey(key);
    if (h) BCryptDestroyHash(h);
    if (ecdsa) BCryptCloseAlgorithmProvider(ecdsa, 0);
    if (sha) BCryptCloseAlgorithmProvider(sha, 0);
    return ok;
}

/* Pauserick.exe.sig: "pauserick-update-signature v1\nversion X\nsignature <128 hex>\n" (tools/pksign.c) */
static int update_parse_sig(const char *t, char *ver, int cap, BYTE sig[64])
{
    static const char head[] = "pauserick-update-signature v1\n";
    const char *v = strstr(t, "\nversion "), *s = strstr(t, "\nsignature ");
    if (strncmp(t, head, sizeof head - 1) || !v || !s) return 0;
    int n = 0;
    for (v += 9; *v && *v != '\n' && *v != '\r' && n < cap - 1; v++) ver[n++] = *v;
    ver[n] = 0;
    s += 11;
    for (int i = 0; i < 64; i++) {
        int hi = hex_val(s[2 * i]);
        if (hi < 0) return 0;
        int lo = hex_val(s[2 * i + 1]);
        if (lo < 0) return 0;
        sig[i] = (BYTE)(hi * 16 + lo);
    }
    return n > 0;
}

/*
 * Downloads and checks the signed update, then swaps it in: the running exe is renamed
 * to Pauserick.old.exe (Windows allows renaming a running program) and the new one takes
 * its name. Runs in a worker thread, or in the elevated helper (--apply-update) when the
 * exe's folder needs administrator rights. Everything is checked in memory before a
 * single byte is written, by the process that writes it.
 */
static int update_apply(const WCHAR *exeUrl, const WCHAR *sigUrl, const WCHAR *version)
{
    char ver[24], sver[24];
    BYTE sig[64];
    DWORD len = 0, err = 0;
    WideCharToMultiByte(CP_UTF8, 0, version, -1, ver, sizeof ver, NULL, NULL);
    char *text = http_get(sigUrl, 4096, &len, &err);
    if (!text) { log_warn(L"Update %s: signature download failed (error %lu)", version, err); return UPD_ERR_NET; }
    int ok = update_parse_sig(text, sver, sizeof sver, sig) && !strcmp(sver, ver);
    HeapFree(GetProcessHeap(), 0, text);
    if (!ok) { log_error(L"Update %s rejected: its signature file is invalid or names another version", version); return UPD_ERR_SIG; }
    BYTE *exe = (BYTE *)http_get(exeUrl, UPDATE_MAX_EXE, &len, &err);
    if (!exe) { log_warn(L"Update %s: download failed (error %lu)", version, err); return UPD_ERR_NET; }
    ok = len > 1024 && exe[0] == 'M' && exe[1] == 'Z' && update_verify(exe, len, ver, sig);
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, exe);
        log_error(L"Update %s rejected: its digital signature does not match (file discarded)", version);
        return UPD_ERR_SIG;
    }
    WCHAR tmp[MAX_PATH], old[MAX_PATH];
    exe_sibling(tmp, g_exe, L".update.tmp");
    exe_sibling(old, g_exe, L".old.exe");
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0, e = 0;
    ok = f != INVALID_HANDLE_VALUE && WriteFile(f, exe, len, &w, NULL) && w == len && FlushFileBuffers(f);
    if (!ok) e = GetLastError();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    HeapFree(GetProcessHeap(), 0, exe);
    if (ok && !MoveFileExW(g_exe, old, MOVEFILE_REPLACE_EXISTING)) { e = GetLastError(); ok = 0; }
    if (ok && !MoveFileExW(tmp, g_exe, 0)) {
        e = GetLastError();
        MoveFileExW(old, g_exe, 0); /* put the current version back */
        ok = 0;
    }
    if (!ok) {
        DeleteFileW(tmp);
        log_error(L"Update %s not installed: could not replace %s (error %lu)", version, g_exe, e);
        return e == ERROR_ACCESS_DENIED ? UPD_ERR_RIGHTS : UPD_ERR_FILE;
    }
    log_info(L"Update %s downloaded, signature verified, installed as %s", version, g_exe);
    return UPD_OK;
}

/* Leftovers of an update (the previous exe, an interrupted download): ours, removed. */
static void update_cleanup(void)
{
    WCHAR p[MAX_PATH];
    exe_sibling(p, g_exe, L".old.exe");
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) DeleteFileW(p);
    exe_sibling(p, g_exe, L".update.tmp");
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) DeleteFileW(p);
}

/*
 * GitHub's API answers at most 60 requests an hour per internet address, and a shared
 * network (VPN, office, a provider's NAT) can use them up. Then the website is asked
 * instead: /releases/latest redirects to the latest release's tag, and a release's files
 * have fixed download addresses. Only the release notes are missing this way.
 */
static void update_fetch_web(UpdInfo *u)
{
    WCHAR base[300], url[600], loc[600], tag[40];
    DWORD len = 0;
    int n = 0, parts[3];
    u->web = 1;
    update_web_base(base, ARRAYSIZE(base));
    wsprintfW(url, L"%s/releases/latest", base);
    if (!http_location(url, loc, ARRAYSIZE(loc), &u->err)) { u->result = UPD_ERR_NET; return; }
    const WCHAR *t = wcsstr(loc, L"/releases/tag/");
    if (!t) { u->result = UPD_UP_TO_DATE; u->err = 404; return; } /* no release published yet */
    for (t += 14; t[n] && t[n] != L'?' && t[n] != L'#' && t[n] != L'/' && n < (int)ARRAYSIZE(tag) - 1; n++) tag[n] = t[n];
    tag[n] = 0;
    if (!ver_parse(tag, parts)) { u->result = UPD_ERR_DATA; return; }
    wsprintfW(u->version, L"%d.%d.%d", parts[0], parts[1], parts[2]);
    if (!ver_newer(u->version)) { u->result = UPD_UP_TO_DATE; return; }
    wsprintfW(u->exeUrl, L"%s/releases/download/%s/Pauserick.exe", base, tag);
    wsprintfW(u->sigUrl, L"%s/releases/download/%s/Pauserick.exe.sig", base, tag);
    char *sig = http_get(u->sigUrl, 4096, &len, &u->err); /* offered only if it is signed */
    if (!sig) { u->result = u->err == 404 ? UPD_ERR_DATA : UPD_ERR_NET; return; }
    HeapFree(GetProcessHeap(), 0, sig);
    u->err = 0;
    u->result = UPD_OK;
}

/* Asks GitHub for the latest release and fills u with what it offers. */
static void update_fetch(UpdInfo *u)
{
    WCHAR url[512];
    DWORD len = 0;
    update_api_url(url, ARRAYSIZE(url));
    char *json = http_get(url, 1u << 20, &len, &u->err);
    if (!json && (u->err == 403 || u->err == 429)) update_fetch_web(u); /* the API's hourly limit */
    else if (!json) u->result = u->err == 404 ? UPD_UP_TO_DATE : UPD_ERR_NET; /* 404: no release published yet */
    else {
        update_parse(json, u);
        HeapFree(GetProcessHeap(), 0, json);
    }
}

static DWORD WINAPI update_check_thread(LPVOID param)
{
    UpdInfo *u = (UpdInfo *)param;
    update_fetch(u);
    if (!PostMessageW(g_main, WM_APP_UPDATE, 0, (LPARAM)u)) {
        HeapFree(GetProcessHeap(), 0, u);
        InterlockedExchange(&g_updBusy, 0);
    }
    return 0;
}

static void dlg_update_link(HWND d);

/* Asks GitHub in a short-lived thread; the answer comes back as WM_APP_UPDATE. */
static void update_check_start(int manual)
{
    if (InterlockedCompareExchange(&g_updBusy, 1, 0)) return;
    UpdInfo *u = (UpdInfo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *u);
    if (u) u->manual = manual;
    HANDLE t = u ? CreateThread(NULL, 64 * 1024, update_check_thread, u, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
    if (t) CloseHandle(t);
    else {
        if (u) HeapFree(GetProcessHeap(), 0, u);
        InterlockedExchange(&g_updBusy, 0);
    }
    if (g_settings) dlg_update_link(g_settings);
}

/* The check's answer, on the main thread. */
static void update_checked(UpdInfo *u)
{
    static int failedBefore; /* while offline, log the failure once, not every 3 hours */
    InterlockedExchange(&g_updBusy, 0);
    int failed = u->result == UPD_ERR_NET || u->result == UPD_ERR_DATA;
    ini_put_ll(L"Update", L"NextCheck", unix_now() + (failed ? UPDATE_RETRY : UPDATE_DAY));
    g_updLast = u->result;
    if (u->result == UPD_OK) {
        WCHAR skip[24];
        GetPrivateProfileStringW(L"Update", L"Skip", L"", skip, ARRAYSIZE(skip), g_ini);
        int skipped = !u->manual && !lstrcmpW(skip, u->version);
        log_info(L"Update check: version %s is available (this is %s)%s%s", u->version, APP_VERSION,
                 u->web ? L", seen on the website because GitHub's API was busy" : L"",
                 skipped ? L", skipped as asked" : L"");
        g_upd = *u;
        if (!skipped) g_updPrompt = 1;
        if (u->manual) PostMessageW(g_main, WM_APP_UPDATE_PROMPT, 0, 0);
    } else if (u->result == UPD_UP_TO_DATE) {
        if (u->err == 404) log_info(L"Update check: no release published yet");
        else               log_info(L"Update check: %s is the latest version", APP_VERSION);
    } else if (u->manual || !failedBefore) {
        if (u->result == UPD_ERR_NET) log_warn(L"Update check failed: GitHub did not answer (error %lu)", u->err);
        else                          log_warn(L"Update check: the latest release has no signed Pauserick.exe, ignored");
    }
    failedBefore = failed;
    if (g_settings) dlg_update_link(g_settings);
    HeapFree(GetProcessHeap(), 0, u);
}

/* Hourly (first 90 s after start): a day since the last check? */
static void update_timer(void)
{
    update_cleanup();
    SetCoalescableTimer(g_main, TIMER_UPDATE, UPDATE_TIMER_MS, NULL, 300000);
    LONGLONG now = unix_now(), next = ini_ll(L"Update", L"NextCheck");
    if (next > now + 2 * UPDATE_DAY) next = 0; /* the clock was set back */
    if (g_cfg.autoUpdate && now >= next) update_check_start(0);
}

/* ------------------------------------------------------------------ tray */

static int g_iconOwned;  /* g_nid.hIcon was created by us and must be destroyed */
static int g_iconPaused; /* g_nid.hIcon is the red "paused" variant (game in front, PC idle) */

static void tray_free_icon(void)
{
    if (g_nid.hIcon && g_iconOwned) DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = NULL;
    g_iconOwned = 0;
}

/* Puts the normal icon, or the one with red bars, in g_nid (the old one is not freed here). */
static void tray_load_icon(int paused)
{
    g_nid.hIcon = NULL;
    g_iconOwned = SUCCEEDED(LoadIconMetric(g_inst, MAKEINTRESOURCEW(paused ? IDI_PAUSED : IDI_APP), LIM_SMALL, &g_nid.hIcon)) &&
                  g_nid.hIcon;
    if (!g_iconOwned) g_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION); /* shared, never destroyed */
    g_iconPaused = paused;
}

/* Red pause bars while a game or a listed app holds the breaks or the PC is idle, the normal icon otherwise. */
static void tray_show_paused(int paused)
{
    paused = paused != 0;
    if (paused == g_iconPaused || !g_nid.hIcon) return; /* no change, or no tray icon yet (tray_add picks it) */
    HICON old = g_nid.hIcon;
    int oldOwned = g_iconOwned;
    tray_load_icon(paused);
    if (g_trayAdded) {
        g_nid.uFlags = NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    if (oldOwned) DestroyIcon(old); /* the shell keeps its own copy */
}

static void tray_add(void)
{
    tray_free_icon(); /* (re)added after an Explorer restart: don't leak the old icon */
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    tray_load_icon(g_gamingNow || (g_pauseFlags & PAUSE_IDLE));
    lstrcpyW(g_nid.szTip, APP_NAME);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (!g_trayAdded) log_warn(L"Tray icon not added yet (taskbar not ready): it will be added when the taskbar starts");
}

static void tray_remove(void)
{
    if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = 0;
    tray_free_icon();
}

/* The tooltip is refreshed only while the mouse hovers the icon. */
static void tray_update_tip(void)
{
    WCHAR s[STATUS_CCH], tip[STATUS_CCH + 16];
    fmt_status(s, FALSE);
    wsprintfW(tip, L"%s\n%s", APP_NAME, s);
    if (lstrcmpW(tip, g_nid.szTip) != 0) {
        lstrcpynW(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip));
        g_nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

static void show_settings(void);

static void tray_menu(void)
{
    WCHAR s[STATUS_CCH];
    POINT pt;
    fmt_status(s, FALSE);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (g_upd.version[0] && !g_updBusy) { /* a newer version was found and not installed yet */
        WCHAR u[96];
        wsprintfW(u, T(S_UPD_TRAY_FMT), g_upd.version);
        AppendMenuW(m, MF_STRING, IDM_UPDATE, u);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    /* offer only the break types that are enabled */
    if (g_cfg.miniEnabled) AppendMenuW(m, MF_STRING, IDM_TAKE_MINI, T(S_TAKE_MINI));
    if (g_cfg.longEnabled) AppendMenuW(m, MF_STRING, IDM_TAKE_LONG, T(S_TAKE_LONG));
    if (g_cfg.miniEnabled || g_cfg.longEnabled) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, T(S_SETTINGS_MENU));
    AppendMenuW(m, MF_STRING | (g_logPath[0] ? 0 : MF_GRAYED), IDM_OPEN_LOG, T(S_OPEN_LOG));
    AppendMenuW(m, MF_STRING, IDM_EXIT, T(S_EXIT));
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ------------------------------------------------- settings: drawing kit */
/*
 * The settings window keeps ordinary dialog controls but paints them in the style
 * of Windows 11: light grey window, white cards with rounded corners, the teal of
 * the icon as accent, switches instead of checkboxes and round-thumb sliders.
 * Everything round is drawn anti-aliased by aa_render(): no GDI+, no extra DLL.
 */
#define C_BG        RGB(243, 243, 243)
#define C_CARD      RGB(255, 255, 255)
#define C_BORDER    RGB(229, 229, 229)
#define C_TEXT      RGB(27, 27, 27)
#define C_TEXT2     RGB(96, 96, 96)
#define C_ACCENT    RGB(15, 118, 110)
#define C_ACCENT_HI RGB(20, 135, 126)
#define C_ACCENT_LO RGB(11, 92, 85)
#define C_OFF       RGB(134, 134, 134)  /* switch outline and knob when off */
#define C_DISABLED  RGB(200, 200, 200)
#define C_TRACK     RGB(200, 200, 200)  /* slider track right of the thumb */
#define C_PAUSED    RGB(196, 43, 28)

static int in_round_box(double x, double y, double x0, double y0, double x1, double y1, double r);

typedef COLORREF (*ShadeFn)(double x, double y, const void *ctx);

/* Paints w x h pixels at x,y: each pixel is the average of 4x4 samples of fn (in DC
 * coordinates), so curved edges are anti-aliased. Written in one blit: no flicker. */
static void aa_render(HDC dc, int x, int y, int w, int h, ShadeFn fn, const void *ctx)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    DWORD *px = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    if (!px) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            unsigned r = 8, g = 8, b = 8; /* +8: rounded average */
            for (int s = 0; s < 16; s++) {
                COLORREF c = fn(x + i + ((s & 3) + 0.5) / 4.0, y + j + ((s >> 2) + 0.5) / 4.0, ctx);
                r += GetRValue(c); g += GetGValue(c); b += GetBValue(c);
            }
            px[j * w + i] = ((r / 16) << 16) | ((g / 16) << 8) | (b / 16);
        }
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, x, y, (DWORD)w, (DWORD)h, 0, 0, 0, (UINT)h, px, &bi, DIB_RGB_COLORS);
    HeapFree(GetProcessHeap(), 0, px);
}

/* Rounded rectangles painted in order over a background; a circle is a square whose
 * radius is half its side. */
typedef struct { double x0, y0, x1, y1, r; COLORREF c; } Shape;
typedef struct { COLORREF bg; int n; Shape s[8]; } Scene;

static void scene_add(Scene *sc, double x0, double y0, double x1, double y1, double r, COLORREF c)
{
    if (sc->n >= (int)ARRAYSIZE(sc->s) || x1 - x0 < 2 * r - 1e-6 || y1 - y0 < 2 * r - 1e-6) return;
    Shape *p = &sc->s[sc->n++];
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1; p->r = r; p->c = c;
}

static void scene_circle(Scene *sc, double cx, double cy, double r, COLORREF c)
{
    scene_add(sc, cx - r, cy - r, cx + r, cy + r, r, c);
}

static COLORREF scene_shade(double x, double y, const void *ctx)
{
    const Scene *sc = (const Scene *)ctx;
    COLORREF c = sc->bg;
    for (int i = 0; i < sc->n; i++) {
        const Shape *p = &sc->s[i];
        if (in_round_box(x, y, p->x0, p->y0, p->x1, p->y1, p->r)) c = p->c;
    }
    return c;
}

/* A white card with a thin border and rounded corners. Only the corners need
 * anti-aliasing; the rest is plain rectangles. */
static void draw_card(HDC dc, const RECT *k, int rad, HBRUSH card, HBRUSH border)
{
    RECT e;
    SetRect(&e, k->left + rad, k->top, k->right - rad, k->top + 1);          FillRect(dc, &e, border);
    SetRect(&e, k->left + rad, k->bottom - 1, k->right - rad, k->bottom);    FillRect(dc, &e, border);
    SetRect(&e, k->left, k->top + rad, k->left + 1, k->bottom - rad);        FillRect(dc, &e, border);
    SetRect(&e, k->right - 1, k->top + rad, k->right, k->bottom - rad);      FillRect(dc, &e, border);
    SetRect(&e, k->left + 1, k->top + rad, k->right - 1, k->bottom - rad);   FillRect(dc, &e, card);
    SetRect(&e, k->left + rad, k->top + 1, k->right - rad, k->top + rad);    FillRect(dc, &e, card);
    SetRect(&e, k->left + rad, k->bottom - rad, k->right - rad, k->bottom - 1); FillRect(dc, &e, card);
    Scene sc = { C_BG, 0, { { 0 } } };
    scene_add(&sc, k->left, k->top, k->right, k->bottom, rad, C_BORDER);
    scene_add(&sc, k->left + 1, k->top + 1, k->right - 1, k->bottom - 1, rad - 1, C_CARD);
    aa_render(dc, k->left, k->top, rad, rad, scene_shade, &sc);
    aa_render(dc, k->right - rad, k->top, rad, rad, scene_shade, &sc);
    aa_render(dc, k->left, k->bottom - rad, rad, rad, scene_shade, &sc);
    aa_render(dc, k->right - rad, k->bottom - rad, rad, rad, scene_shade, &sc);
}

/* A Windows 11 style switch in the w x h box at x,y, inside a margin of h/5 on every
 * side that holds the keyboard focus ring. */
static void draw_switch(HDC dc, int x, int y, int w, int h, int on, int enabled, int hot, int focus, COLORREF bg)
{
    Scene sc = { bg, 0, { { 0 } } };
    double r = h / 2.0, line = h / 20.0 < 1 ? 1 : h / 20.0, knob = h * (hot && enabled ? 0.35 : 0.3), m = h / 5.0;
    if (focus) {
        scene_add(&sc, x - m, y - m, x + w + m, y + h + m, r + m, C_TEXT);
        scene_add(&sc, x - m + 2 * line, y - m + 2 * line, x + w + m - 2 * line, y + h + m - 2 * line, r + m - 2 * line, bg);
    }
    if (on) {
        scene_add(&sc, x, y, x + w, y + h, r, !enabled ? C_DISABLED : hot ? C_ACCENT_HI : C_ACCENT);
        scene_circle(&sc, x + w - r, y + r, knob, RGB(255, 255, 255));
    } else {
        COLORREF edge = !enabled ? C_DISABLED : hot ? RGB(90, 90, 90) : C_OFF;
        scene_add(&sc, x, y, x + w, y + h, r, edge);
        scene_add(&sc, x + line, y + line, x + w - line, y + h - line, r - line,
                  hot && enabled ? RGB(240, 240, 240) : RGB(255, 255, 255));
        scene_circle(&sc, x + r, y + r, knob, edge);
    }
    int mi = (int)m + 1;
    aa_render(dc, x - mi, y - mi, w + 2 * mi, h + 2 * mi, scene_shade, &sc);
}

/* ---- flags for the language list (3:2, drawn, no image resources) ---- */

#define ABSD(v) ((v) < 0 ? -(v) : (v))

/* Union Jack on a 60 x 30 grid (stretched to the box): white and red crosses over
 * white and counterchanged red diagonals (broad white uppermost at the hoist). */
static COLORREF uk_color(double u, double v)
{
    const COLORREF blue = RGB(1, 33, 105), red = RGB(200, 16, 46), white = RGB(255, 255, 255);
    double du = u - 30, dv = v - 15;
    if (ABSD(du) < 3 || ABSD(dv) < 3) return red;
    if (ABSD(du) < 5 || ABSD(dv) < 5) return white;
    int top = v < 15, left = u < 30;
    /* signed distance to the diagonal of this quarter: top-left/bottom-right or the other one */
    double d = top == left ? (u - 2 * v) / 2.2360679775 : (u + 2 * v - 60) / 2.2360679775;
    if (ABSD(d) >= 3) return blue;
    int redSide = top == left ? (left ? d < 0 : d > 0) : (left ? d > 0 : d < 0);
    return redSide && ABSD(d) < 2 ? red : white;
}

typedef struct { double x, y, w, h; int lang; COLORREF bg; } FlagCtx;

static COLORREF flag_shade(double px, double py, const void *ctx)
{
    const FlagCtx *f = (const FlagCtx *)ctx;
    double r = f->h / 7.0, e = 0.8;
    if (!in_round_box(px, py, f->x, f->y, f->x + f->w, f->y + f->h, r)) return f->bg;
    double u = (px - f->x) / f->w, v = (py - f->y) / f->h;
    COLORREF c = f->lang == LANG_IT ? (u < 1 / 3.0 ? RGB(0, 146, 70) : u < 2 / 3.0 ? RGB(244, 245, 240) : RGB(206, 43, 55))
                                    : uk_color(u * 60, v * 30);
    if (!in_round_box(px, py, f->x + e, f->y + e, f->x + f->w - e, f->y + f->h - e, r > e ? r - e : 0))
        c = RGB(GetRValue(c) * 3 / 4, GetGValue(c) * 3 / 4, GetBValue(c) * 3 / 4); /* hairline edge: white parts stand out */
    return c;
}

static void draw_flag(HDC dc, int x, int y, int h, int lang, COLORREF bg)
{
    FlagCtx f = { x, y, h * 3 / 2, h, lang, bg };
    aa_render(dc, x, y, h * 3 / 2, h, flag_shade, &f);
}

/* GitHub's mark (the "mark-github" octicon): one outline in a 16 x 16 box, in hundredths,
 * flattened from its SVG path by tools/github-mark.py. */
static const short GH_MARK[][2] = { /* 173 points, tools/github-mark.py */
    {800, 0}, {718, 4}, {639, 16}, {562, 36}, {489, 63}, {419, 97}, {353, 137}, {291, 183},
    {234, 234}, {183, 291}, {137, 353}, {97, 419}, {63, 489}, {36, 562}, {16, 639}, {4, 718},
    {0, 800}, {3, 866}, {10, 930}, {23, 992}, {41, 1053}, {63, 1112}, {89, 1168}, {120, 1221},
    {154, 1272}, {192, 1321}, {234, 1366}, {279, 1407}, {327, 1445}, {378, 1480}, {432, 1510}, {488, 1537},
    {547, 1559}, {572, 1559}, {590, 1550}, {599, 1537}, {602, 1521}, {601, 1372}, {532, 1380}, {476, 1379},
    {432, 1370}, {397, 1356}, {371, 1337}, {353, 1317}, {340, 1296}, {332, 1278}, {320, 1253}, {300, 1220},
    {276, 1187}, {250, 1165}, {229, 1151}, {214, 1133}, {212, 1125}, {217, 1119}, {229, 1114}, {249, 1112},
    {272, 1114}, {293, 1121}, {312, 1131}, {328, 1144}, {355, 1171}, {372, 1194}, {401, 1232}, {432, 1258},
    {465, 1274}, {498, 1280}, {530, 1280}, {559, 1276}, {605, 1260}, {613, 1224}, {625, 1195}, {639, 1171},
    {656, 1154}, {589, 1144}, {524, 1127}, {463, 1101}, {434, 1084}, {407, 1064}, {383, 1040}, {360, 1013},
    {341, 982}, {324, 947}, {310, 907}, {300, 863}, {294, 814}, {292, 759}, {298, 697}, {314, 640},
    {340, 589}, {374, 544}, {366, 518}, {359, 472}, {358, 442}, {361, 409}, {369, 372}, {382, 332},
    {395, 330}, {435, 334}, {465, 343}, {503, 358}, {549, 381}, {602, 414}, {651, 402}, {700, 394},
    {751, 389}, {802, 387}, {853, 389}, {904, 394}, {953, 402}, {1002, 414}, {1055, 381}, {1101, 357},
    {1139, 342}, {1169, 334}, {1193, 331}, {1209, 330}, {1222, 332}, {1235, 372}, {1243, 409}, {1246, 442},
    {1245, 472}, {1238, 518}, {1230, 544}, {1264, 589}, {1290, 640}, {1306, 696}, {1312, 759}, {1310, 814},
    {1304, 863}, {1294, 908}, {1280, 947}, {1263, 983}, {1243, 1014}, {1221, 1041}, {1196, 1064}, {1169, 1084},
    {1141, 1101}, {1079, 1127}, {1014, 1144}, {947, 1154}, {968, 1177}, {985, 1209}, {997, 1251}, {1001, 1302},
    {1000, 1522}, {1003, 1538}, {1012, 1552}, {1030, 1560}, {1055, 1560}, {1157, 1517}, {1253, 1461}, {1340, 1392},
    {1416, 1312}, {1481, 1222}, {1532, 1123}, {1570, 1019}, {1592, 911}, {1600, 800}, {1596, 718}, {1584, 639},
    {1564, 562}, {1537, 489}, {1503, 419}, {1463, 353}, {1417, 291}, {1366, 234}, {1309, 183}, {1247, 137},
    {1181, 97}, {1111, 63}, {1038, 36}, {961, 16}, {882, 4},
};

typedef struct { Scene bg; double x, y, k; COLORREF fg; } MarkCtx; /* k: pixels per hundredth */

static COLORREF mark_shade(double px, double py, const void *ctx)
{
    const MarkCtx *m = (const MarkCtx *)ctx;
    double u = (px - m->x) / m->k, v = (py - m->y) / m->k;
    if ((u - 800) * (u - 800) + (v - 800) * (v - 800) < 810.0 * 810.0) { /* the mark fits in this circle */
        int in = 0, n = (int)ARRAYSIZE(GH_MARK);
        for (int i = 0, j = n - 1; i < n; j = i++) { /* even-odd: edges crossed by a ray to the right */
            double yi = GH_MARK[i][1], yj = GH_MARK[j][1];
            if ((yi > v) != (yj > v) && u < GH_MARK[j][0] + (v - yj) * (GH_MARK[i][0] - GH_MARK[j][0]) / (yi - yj))
                in = !in;
        }
        if (in) return m->fg;
    }
    return scene_shade(px, py, &m->bg);
}

/* ---- slider: Pauserick's own control, so the thumb can be a round knob ----
 * It answers the trackbar messages the dialog uses (TBM_SETRANGE / SETPOS / GETPOS)
 * and reports to the parent with WM_HSCROLL like a trackbar: TB_THUMBTRACK while
 * dragging, TB_ENDTRACK when the mouse button or the key is released. Mouse (click
 * anywhere, drag), wheel and keys (arrows, Page Up/Down, Home/End) all work. */

#define CLS_SLIDER L"PauserickSlider"
enum { SLX_MAX = 0, SLX_POS = 4, SLX_STATE = 8, SLX_WHEEL = 12, SLX_BYTES = 16 }; /* window extra bytes */
enum { SLS_HOT = 1, SLS_DRAG = 2 };

static int  sl_get(HWND h, int off)        { return (int)GetWindowLongW(h, off); }
static void sl_put(HWND h, int off, int v) { SetWindowLongW(h, off, v); }

static void sl_notify(HWND h, int code)
{
    SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(code, sl_get(h, SLX_POS)), (LPARAM)h);
}

static void sl_set(HWND h, int pos, int code)
{
    pos = clampi(pos, 0, sl_get(h, SLX_MAX));
    if (pos == sl_get(h, SLX_POS)) return;
    sl_put(h, SLX_POS, pos);
    InvalidateRect(h, NULL, FALSE);
    sl_notify(h, code);
}

/* thumb radius and the two ends of the track, in client pixels (room left for the focus ring) */
static void sl_geometry(HWND h, double *r, double *x0, double *x1)
{
    RECT rc;
    GetClientRect(h, &rc);
    double rad = MulDiv(10, (int)GetDpiForWindow(h), 96);
    if (rad > rc.bottom / 2.0 - 3) rad = rc.bottom / 2.0 - 3;
    if (rad < 3) rad = 3;
    *r = rad;
    *x0 = rad + 3;
    *x1 = rc.right - rad - 3;
}

static void sl_follow_mouse(HWND h, int x)
{
    double r, x0, x1;
    sl_geometry(h, &r, &x0, &x1);
    double t = x1 > x0 ? (x - x0) / (x1 - x0) : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    sl_set(h, (int)(t * sl_get(h, SLX_MAX) + 0.5), TB_THUMBTRACK);
}

/* The colour behind a child control, as its parent paints it (WM_CTLCOLORSTATIC). */
static COLORREF parent_bg(HWND h, HDC dc)
{
    HBRUSH br = (HBRUSH)SendMessageW(GetParent(h), WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h);
    LOGBRUSH lb;
    if (br && GetObjectW(br, sizeof lb, &lb) == sizeof lb && lb.lbStyle == BS_SOLID) return lb.lbColor;
    return GetSysColor(COLOR_BTNFACE);
}

static void sl_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    double r, x0, x1;
    sl_geometry(h, &r, &x0, &x1);
    int max = sl_get(h, SLX_MAX), pos = sl_get(h, SLX_POS), st = sl_get(h, SLX_STATE), en = IsWindowEnabled(h);
    int focus = GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    double cy = rc.bottom / 2.0, tx = max > 0 ? x0 + (x1 - x0) * pos / max : x0, t = r / 5.0;
    COLORREF fill = en ? C_ACCENT : C_DISABLED;
    Scene sc = { parent_bg(h, dc), 0, { { 0 } } };
    scene_add(&sc, x0 - t, cy - t, x1 + t, cy + t, t, en ? C_TRACK : RGB(226, 226, 226));
    scene_add(&sc, x0 - t, cy - t, tx + t, cy + t, t, fill);
    if (focus) {
        scene_circle(&sc, tx, cy, r + 2.5, C_TEXT);
        scene_circle(&sc, tx, cy, r + 1, sc.bg);
    }
    scene_circle(&sc, tx, cy, r, en ? RGB(204, 204, 204) : RGB(222, 222, 222)); /* thumb edge */
    scene_circle(&sc, tx, cy, r - (r > 8 ? 1.2 : 1), RGB(255, 255, 255));
    scene_circle(&sc, tx, cy, r * ((st & SLS_DRAG) ? 0.42 : (st & SLS_HOT) && en ? 0.62 : 0.52),
                 (st & SLS_HOT) && en ? C_ACCENT_HI : fill);
    aa_render(dc, 0, 0, rc.right, rc.bottom, scene_shade, &sc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK slider_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case TBM_SETRANGE: /* MAKELPARAM(0, max) */
        sl_put(h, SLX_MAX, HIWORD(lp) > LOWORD(lp) ? HIWORD(lp) - LOWORD(lp) : 0);
        sl_put(h, SLX_POS, clampi(sl_get(h, SLX_POS), 0, sl_get(h, SLX_MAX)));
        if (wp) InvalidateRect(h, NULL, FALSE);
        return 0;
    case TBM_SETPOS:
        sl_put(h, SLX_POS, clampi((int)lp, 0, sl_get(h, SLX_MAX)));
        if (wp) InvalidateRect(h, NULL, FALSE);
        return 0;
    case TBM_GETPOS:
        return sl_get(h, SLX_POS);
    case TBM_SETPAGESIZE:
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_UPDATEUISTATE: {
        LRESULT res = DefWindowProcW(h, m, wp, lp);
        InvalidateRect(h, NULL, FALSE); /* the focus ring follows keyboard / mouse use */
        return res;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        sl_put(h, SLX_STATE, sl_get(h, SLX_STATE) | SLS_DRAG);
        InvalidateRect(h, NULL, FALSE);
        sl_follow_mouse(h, (short)LOWORD(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (!(sl_get(h, SLX_STATE) & SLS_HOT)) {
            TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 };
            TrackMouseEvent(&te);
            sl_put(h, SLX_STATE, sl_get(h, SLX_STATE) | SLS_HOT);
            InvalidateRect(h, NULL, FALSE);
        }
        if (sl_get(h, SLX_STATE) & SLS_DRAG) sl_follow_mouse(h, (short)LOWORD(lp));
        return 0;
    case WM_MOUSELEAVE:
        sl_put(h, SLX_STATE, sl_get(h, SLX_STATE) & ~SLS_HOT);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (sl_get(h, SLX_STATE) & SLS_DRAG) {
            sl_put(h, SLX_STATE, sl_get(h, SLX_STATE) & ~SLS_DRAG); /* first, so ReleaseCapture below is a no-op here */
            if (m == WM_LBUTTONUP) ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
            sl_notify(h, TB_ENDTRACK);
        }
        return 0;
    case WM_KEYDOWN: {
        int p = sl_get(h, SLX_POS);
        switch (wp) {
        case VK_LEFT: case VK_DOWN: sl_set(h, p - 1, TB_LINEUP);   return 0;
        case VK_RIGHT: case VK_UP:  sl_set(h, p + 1, TB_LINEDOWN); return 0;
        case VK_NEXT:  sl_set(h, p - 3, TB_PAGEUP);   return 0;
        case VK_PRIOR: sl_set(h, p + 3, TB_PAGEDOWN); return 0;
        case VK_HOME:  sl_set(h, 0, TB_TOP);          return 0;
        case VK_END:   sl_set(h, sl_get(h, SLX_MAX), TB_BOTTOM); return 0;
        }
        break;
    }
    case WM_KEYUP:
        if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN || wp == VK_NEXT || wp == VK_PRIOR ||
            wp == VK_HOME || wp == VK_END) {
            sl_notify(h, TB_ENDTRACK);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: { /* up = more */
        int acc = sl_get(h, SLX_WHEEL) + GET_WHEEL_DELTA_WPARAM(wp), steps = acc / WHEEL_DELTA;
        sl_put(h, SLX_WHEEL, acc - steps * WHEEL_DELTA);
        if (steps) sl_set(h, sl_get(h, SLX_POS) + steps, steps > 0 ? TB_LINEDOWN : TB_LINEUP);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        sl_paint(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void slider_register(void)
{
    WNDCLASSEXW wc = { sizeof wc };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = slider_proc;
    wc.cbWndExtra = SLX_BYTES;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLS_SLIDER;
    RegisterClassExW(&wc);
}

/* -------------------------------------------------------- settings dialog */

/* Every change is saved immediately; there is no Save button. */

#define DLG_TIMER_STATUS 1
#define DLG_TIMER_SAVE   2
#define DLG_SAVE_DELAY   400 /* ms after the last slider movement */
#define WM_APP_DLG_DPI   (WM_APP + 20) /* posted after a DPI change: fonts and painting follow */

static int g_dlgDirty; /* a slider moved but its value is not saved yet */

/* Time sliders: each position is one of these values (fine steps where they matter). */
static const int V_MINI_EVERY[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 75, 90, 105, 120 };
static const int V_MINI_DUR[]   = { 5, 10, 15, 20, 25, 30, 40, 50, 60, 75, 90, 120, 150, 180, 240, 300 };
static const int V_LONG_EVERY[] = { 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80, 90, 100, 110, 120, 150, 180, 240 };
static const int V_LONG_DUR[]   = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 20, 25, 30, 40, 45, 60 };
static const int V_WARN[]       = { 0, 5, 10, 15, 20, 30, 45, 60, 90, 120 };
static const int V_IDLE[]       = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 30, 45, 60 };

enum { UNIT_MIN, UNIT_SEC };

typedef struct {
    int ctl, lbl;     /* trackbar and the label showing its value */
    const int *vals;
    int n, unit;
    size_t field;     /* offsetof(Settings, ...) */
} Slider;

#define SLIDER(c, l, v, u, f) { c, l, v, (int)ARRAYSIZE(v), u, offsetof(Settings, f) }
static const Slider SLIDERS[] = {
    SLIDER(IDC_MINI_EVERY, IDC_VAL_MINI_EVERY, V_MINI_EVERY, UNIT_MIN, miniEveryMin),
    SLIDER(IDC_MINI_DUR,   IDC_VAL_MINI_DUR,   V_MINI_DUR,   UNIT_SEC, miniDurSec),
    SLIDER(IDC_LONG_EVERY, IDC_VAL_LONG_EVERY, V_LONG_EVERY, UNIT_MIN, longEveryMin),
    SLIDER(IDC_LONG_DUR,   IDC_VAL_LONG_DUR,   V_LONG_DUR,   UNIT_MIN, longDurMin),
    SLIDER(IDC_WARN,       IDC_VAL_WARN,       V_WARN,       UNIT_SEC, warnSec),
    SLIDER(IDC_IDLE,       IDC_VAL_IDLE,       V_IDLE,       UNIT_MIN, idlePauseMin),
};
#define NSLIDERS ((int)ARRAYSIZE(SLIDERS))

static int g_dlgVal[NSLIDERS]; /* value currently chosen on each slider */

static int *cfg_field(Settings *s, const Slider *sl) { return (int *)((char *)s + sl->field); }

/* "45 s", "1 min 30 s", "45 min", "1 h 15 min", "Off" */
static void fmt_amount(WCHAR *b, int v, int unit)
{
    if (v == 0) { lstrcpyW(b, T(S_OFF)); return; }
    int m = unit == UNIT_SEC ? v / 60 : v, s = unit == UNIT_SEC ? v % 60 : 0, h = m / 60;
    WCHAR *p = b;
    m %= 60;
    *p = 0;
    if (h) p += wsprintfW(p, L"%d h ", h);
    if (m) p += wsprintfW(p, L"%d min ", m);
    if (s) p += wsprintfW(p, L"%d s ", s);
    if (p > b) p[-1] = 0; /* trailing space */
}

static int slider_index(const Slider *sl, int v)
{
    int best = 0;
    for (int i = 1; i < sl->n; i++) {
        int a = sl->vals[i] - v, b = sl->vals[best] - v;
        if ((a < 0 ? -a : a) < (b < 0 ? -b : b)) best = i;
    }
    return best;
}

static void slider_label(HWND d, int i)
{
    WCHAR b[32];
    fmt_amount(b, g_dlgVal[i], SLIDERS[i].unit);
    SetDlgItemTextW(d, SLIDERS[i].lbl, b);
}

static void sliders_init(HWND d)
{
    for (int i = 0; i < NSLIDERS; i++) {
        const Slider *sl = &SLIDERS[i];
        HWND t = GetDlgItem(d, sl->ctl);
        g_dlgVal[i] = *cfg_field(&g_cfg, sl); /* an exact ini value is kept until the slider moves */
        SendMessageW(t, TBM_SETRANGE, FALSE, MAKELPARAM(0, sl->n - 1));
        SendMessageW(t, TBM_SETPAGESIZE, 0, 3);
        SendMessageW(t, TBM_SETPOS, TRUE, slider_index(sl, g_dlgVal[i]));
        slider_label(d, i);
    }
}

/* Card switches keep their full text (read by screen readers) but show only the switch. */
static const struct { int ctl, str; } DLG_TEXTS[] = {
    { IDC_MINI_ENABLE, S_ENABLE_MINI },   { IDC_LBL_MINI_EVERY, S_EVERY },  { IDC_LBL_MINI_DUR, S_DURATION },
    { IDC_LONG_ENABLE, S_ENABLE_LONG },   { IDC_LBL_LONG_EVERY, S_EVERY },  { IDC_LBL_LONG_DUR, S_DURATION },
    { IDC_IDLE_ENABLE, S_IDLE },          { IDC_LBL_IDLE_AFTER, S_AFTER },  { IDC_LBL_RESUME, S_WHEN_BACK },
    { IDC_AUTOSTART, S_AUTOSTART },       { IDC_STRICT, S_STRICT },         { IDC_SOUND, S_SOUND },
    { IDC_AUTOUPDATE, S_AUTOUPDATE },     { IDC_LBL_WARN, S_WARN },
    { IDC_GAMING, S_GAMING },             { IDC_LBL_GAMES, S_GAMES_HINT },  { IDC_PICK_GAMES, S_PICK },
    { IDC_LBL_GAMES_AUTO, S_GAMES_AUTO },
    { IDC_LBL_LANG, S_LANGUAGE },         { IDCANCEL, S_CLOSE },            { IDC_GITHUB, S_GITHUB },
};

static int g_dlgLoading; /* filling the games field ourselves: ignore EN_CHANGE */

/* ---- painting: header, cards, switches, accent button, language list with flags ---- */

typedef struct { int grp; WCHAR icon; int title, desc; } Card; /* icon: Segoe Fluent Icons / MDL2 code point */
static const Card CARDS[] = {
    { IDC_GRP_MINI,    0xE7B3, S_MINI_BREAK,   S_MINI_DESC },   /* eye */
    { IDC_GRP_LONG,    0xE805, S_BREAK,        S_LONG_DESC },   /* walking person */
    { IDC_GRP_IDLE,    0xE708, S_IDLE_GROUP,   S_IDLE_DESC },   /* moon */
    { IDC_GRP_GENERAL, 0xE713, S_GENERAL,      -1 },            /* gear */
    { IDC_GRP_GAMING,  0xE7FC, S_GAMING_GROUP, S_GAMING_DESC }, /* game controller */
};
#define ICON_GLOBE 0xE774

typedef struct {
    UINT dpi;
    HFONT fTitle, fCard, fValue, fIcon;
    HBRUSH brBg, brCard, brBorder;
    HICON logo;
    HBITMAP cache;      /* everything that doesn't change by itself, painted once */
    int cacheW, cacheH, dirty, inPaint;
} DlgUi;
static DlgUi g_ui;

static int px96(int v) { return MulDiv(v, (int)g_ui.dpi, 96); }

static RECT dlu_rect(HWND d, int x, int y, int w, int h)
{
    RECT r = { x, y, x + w, y + h };
    MapDialogRect(d, &r);
    return r;
}

static int dlu_x(HWND d, int v) { return dlu_rect(d, 0, 0, v, 0).right; }
static int dlu_y(HWND d, int v) { return dlu_rect(d, 0, 0, 0, v).bottom; }

static RECT child_rect(HWND d, int id)
{
    RECT r = { 0, 0, 0, 0 };
    HWND c = GetDlgItem(d, id);
    if (c) {
        GetWindowRect(c, &r);
        MapWindowPoints(NULL, d, (POINT *)&r, 2);
    }
    return r;
}

static int is_card_switch(int id) { return id == IDC_MINI_ENABLE || id == IDC_LONG_ENABLE || id == IDC_IDLE_ENABLE || id == IDC_GAMING; }
static int is_row_switch(int id)  { return id == IDC_AUTOSTART || id == IDC_STRICT || id == IDC_SOUND || id == IDC_AUTOUPDATE; }

/* Windows 11's icon font, or Windows 10's; NULL if neither is installed (no icons then). */
static HFONT icon_font(int px)
{
    static const WCHAR *const faces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    HDC dc = GetDC(NULL);
    for (int i = 0; i < (int)ARRAYSIZE(faces); i++) {
        HFONT f = make_font(px, FW_NORMAL, faces[i], CLEARTYPE_QUALITY);
        WCHAR got[LF_FACESIZE] = L"";
        HGDIOBJ old = SelectObject(dc, f);
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        if (!lstrcmpiW(got, faces[i])) { ReleaseDC(NULL, dc); return f; }
        DeleteObject(f);
    }
    ReleaseDC(NULL, dc);
    return NULL;
}

static void ui_free_fonts(void)
{
    HFONT *f[] = { &g_ui.fTitle, &g_ui.fCard, &g_ui.fValue, &g_ui.fIcon };
    for (int i = 0; i < (int)ARRAYSIZE(f); i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
    if (g_ui.logo) { DestroyIcon(g_ui.logo); g_ui.logo = NULL; }
}

/* Fonts, logo and list heights for the window's current DPI. */
static void ui_fonts(HWND d)
{
    ui_free_fonts();
    g_ui.dpi = GetDpiForWindow(d);
    if (!g_ui.dpi) g_ui.dpi = 96;
    g_ui.fTitle = make_font(px96(26), FW_SEMIBOLD, L"Segoe UI", CLEARTYPE_QUALITY);
    g_ui.fCard  = make_font(px96(15), FW_SEMIBOLD, L"Segoe UI", CLEARTYPE_QUALITY);
    g_ui.fValue = make_font(px96(12), FW_SEMIBOLD, L"Segoe UI", CLEARTYPE_QUALITY);
    g_ui.fIcon  = icon_font(px96(16));
    if (FAILED(LoadIconWithScaleDown(g_inst, MAKEINTRESOURCEW(IDI_APP), px96(48), px96(48), &g_ui.logo))) g_ui.logo = NULL;
    for (int i = 0; i < NSLIDERS; i++) SendDlgItemMessageW(d, SLIDERS[i].lbl, WM_SETFONT, (WPARAM)g_ui.fValue, FALSE);
    SendDlgItemMessageW(d, IDC_LANG, CB_SETITEMHEIGHT, (WPARAM)-1, px96(22));
    SendDlgItemMessageW(d, IDC_LANG, CB_SETITEMHEIGHT, 0, px96(22));
    g_ui.dirty = 1;
}

static void ui_init(HWND d)
{
    ZeroMemory(&g_ui, sizeof g_ui);
    g_ui.brBg = CreateSolidBrush(C_BG);
    g_ui.brCard = CreateSolidBrush(C_CARD);
    g_ui.brBorder = CreateSolidBrush(C_BORDER);
    ui_fonts(d);
}

static void ui_free(void)
{
    ui_free_fonts();
    if (g_ui.brBg) DeleteObject(g_ui.brBg);
    if (g_ui.brCard) DeleteObject(g_ui.brCard);
    if (g_ui.brBorder) DeleteObject(g_ui.brBorder);
    if (g_ui.cache) DeleteObject(g_ui.cache);
    ZeroMemory(&g_ui, sizeof g_ui);
}

static int ui_on_card(HWND d, HWND c)
{
    RECT r;
    GetWindowRect(c, &r);
    MapWindowPoints(NULL, d, (POINT *)&r, 2);
    POINT p = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    for (int i = 0; i < (int)ARRAYSIZE(CARDS); i++) {
        RECT k = child_rect(d, CARDS[i].grp);
        if (PtInRect(&k, p)) return 1;
    }
    return 0;
}

/* Static texts, checkboxes and sliders take the colour of the card or window under them. */
static HBRUSH ui_ctlcolor(HWND d, HDC dc, HWND c)
{
    int card = ui_on_card(d, c), id = GetDlgCtrlID(c);
    COLORREF fg = id == IDC_LBL_GAMES_AUTO ? C_TEXT2 : C_TEXT;
    for (int i = 0; i < NSLIDERS; i++)
        if (SLIDERS[i].lbl == id) fg = C_ACCENT; /* slider values */
    SetTextColor(dc, fg);
    SetBkColor(dc, card ? C_CARD : C_BG);
    return card ? g_ui.brCard : g_ui.brBg;
}

/* How far the countdown to the next break is: 0 = no break enabled, 1 = counting,
 * 2 = on hold (locked, idle, a game in front), 3 = a break is running. */
static int ui_progress(double *frac)
{
    *frac = 0;
    if (g_state == ST_BREAK) { *frac = 1; return 3; }
    int type;
    ULONGLONG at;
    if (!next_break(&type, &at)) return 0;
    int held = g_pauseFlags || g_gamingNow;
    ULONGLONG now = g_pauseFlags ? g_pausedSince : now_ms(); /* frozen while paused */
    ULONGLONG every = (ULONGLONG)(type == BT_LONG ? g_cfg.longEveryMin : g_cfg.miniEveryMin) * 60000;
    ULONGLONG left = at > now ? at - now : 0;
    if (left > every) left = every;
    *frac = every ? 1.0 - (double)left / (double)every : 0;
    return held ? 2 : 1;
}

/* the status dot left of the status text and the progress bar under it */
static RECT ui_dot_rect(HWND d)
{
    RECT s = child_rect(d, IDC_STATUS), r;
    int rad = px96(4), cx = s.left - px96(9), cy = (s.top + s.bottom) / 2;
    SetRect(&r, cx - rad - 1, cy - rad - 1, cx + rad + 1, cy + rad + 1);
    return r;
}

static RECT ui_bar_rect(HWND d)
{
    RECT r = dlu_rect(d, 42, 33, 210, 0);
    r.bottom = r.top + px96(4);
    return r;
}

static void ui_paint_dynamic(HWND d, HDC dc)
{
    double frac;
    int st = ui_progress(&frac);
    RECT o = ui_dot_rect(d), b = ui_bar_rect(d);
    Scene dot = { C_BG, 0, { { 0 } } };
    scene_circle(&dot, (o.left + o.right) / 2.0, (o.top + o.bottom) / 2.0, (o.right - o.left) / 2.0 - 1,
                 st == 2 ? C_PAUSED : st ? C_ACCENT : C_OFF);
    aa_render(dc, o.left, o.top, o.right - o.left, o.bottom - o.top, scene_shade, &dot);
    Scene bar = { C_BG, 0, { { 0 } } };
    double t = (b.bottom - b.top) / 2.0;
    scene_add(&bar, b.left, b.top, b.right, b.bottom, t, RGB(222, 222, 222));
    if (st) scene_add(&bar, b.left, b.top, b.left + 2 * t + (b.right - b.left - 2 * t) * frac, b.bottom, t,
                      st == 2 ? C_OFF : C_ACCENT);
    aa_render(dc, b.left, b.top, b.right - b.left, b.bottom - b.top, scene_shade, &bar);
}

static void ui_paint_static(HWND d, HDC dc, int w, int h)
{
    RECT all = { 0, 0, w, h };
    FillRect(dc, &all, g_ui.brBg);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = (HFONT)SendMessageW(d, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = SelectObject(dc, font);

    /* header: logo, name, version */
    RECT r = dlu_rect(d, 8, 7, 0, 0);
    if (g_ui.logo) DrawIconEx(dc, r.left, r.top, g_ui.logo, px96(48), px96(48), 0, NULL, DI_NORMAL);
    r = dlu_rect(d, 42, 2, 200, 18);
    SelectObject(dc, g_ui.fTitle);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, APP_NAME, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
    r = dlu_rect(d, 250, 6, 117, 10); /* left of the GitHub mark and its focus ring */
    SelectObject(dc, font);
    SetTextColor(dc, C_TEXT2);
    DrawTextW(dc, L"v" APP_VERSION, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_RIGHT);

    /* cards: icon, title, one-line description (left of the card's switch, if any) */
    for (int i = 0; i < (int)ARRAYSIZE(CARDS); i++) {
        const Card *c = &CARDS[i];
        RECT k = child_rect(d, c->grp);
        draw_card(dc, &k, px96(7), g_ui.brCard, g_ui.brBorder);
        int right = k.right - dlu_x(d, 8);
        for (int s = 0; s < (int)ARRAYSIZE(DLG_TEXTS); s++) {
            RECT sw = child_rect(d, DLG_TEXTS[s].ctl);
            if (is_card_switch(DLG_TEXTS[s].ctl) && sw.left >= k.left && sw.right <= k.right && sw.top >= k.top &&
                sw.bottom <= k.bottom)
                right = sw.left - px96(6);
        }
        RECT row = { k.left + dlu_x(d, 10), k.top + dlu_y(d, 4), k.left + dlu_x(d, 26), k.top + dlu_y(d, 16) };
        if (g_ui.fIcon) {
            SelectObject(dc, g_ui.fIcon);
            SetTextColor(dc, C_ACCENT);
            DrawTextW(dc, &c->icon, 1, &row, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
        }
        row.left = row.right;
        row.right = right;
        SelectObject(dc, g_ui.fCard);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, T(c->title), -1, &row, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS);
        if (c->desc >= 0) {
            RECT ds = { row.left, k.top + dlu_y(d, 17), right, k.top + dlu_y(d, 27) };
            SelectObject(dc, font);
            SetTextColor(dc, C_TEXT2);
            DrawTextW(dc, T(c->desc), -1, &ds, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    /* footer: a globe before "Language" */
    if (g_ui.fIcon) {
        RECT lab = child_rect(d, IDC_LBL_LANG);
        RECT g = { lab.left - px96(24), lab.top - px96(4), lab.left, lab.bottom + px96(4) };
        WCHAR globe = ICON_GLOBE;
        SelectObject(dc, g_ui.fIcon);
        SetTextColor(dc, C_TEXT2);
        DrawTextW(dc, &globe, 1, &g, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
    }
    SelectObject(dc, oldFont);
}

/* Paints the window into dc (its own WM_PAINT, or a child asking for its background). */
static void ui_paint(HWND d, HDC dc, const RECT *clip)
{
    if (!g_ui.brBg) return;
    RECT rc;
    GetClientRect(d, &rc);
    if (!g_ui.cache || g_ui.dirty || g_ui.cacheW != rc.right || g_ui.cacheH != rc.bottom) {
        if (g_ui.cache) DeleteObject(g_ui.cache);
        HDC wdc = GetDC(d);
        g_ui.cache = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        ReleaseDC(d, wdc);
        g_ui.cacheW = rc.right;
        g_ui.cacheH = rc.bottom;
        g_ui.dirty = 0;
        if (!g_ui.cache) return;
        HDC mem = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(mem, g_ui.cache);
        ui_paint_static(d, mem, rc.right, rc.bottom);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    HDC mem = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(mem, g_ui.cache);
    BitBlt(dc, clip->left, clip->top, clip->right - clip->left, clip->bottom - clip->top, mem, clip->left, clip->top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ui_paint_dynamic(d, dc);
}

/* Checkboxes drawn as switches; the Close button filled with the accent colour. */
static LRESULT ui_custom_draw(HWND d, const NMCUSTOMDRAW *cd)
{
    (void)d;
    int id = (int)cd->hdr.idFrom, card = is_card_switch(id);
    if (cd->dwDrawStage != CDDS_PREPAINT || !g_ui.brCard) return CDRF_DODEFAULT;
    HWND b = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    int en = IsWindowEnabled(b), keys = (cd->uItemState & CDIS_FOCUS) && (cd->uItemState & CDIS_SHOWKEYBOARDCUES);
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(b, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    if (id == IDCANCEL) {
        UINT s = cd->uItemState;
        COLORREF c = !en ? C_DISABLED : (s & CDIS_SELECTED) ? C_ACCENT_LO : (s & CDIS_HOT) ? C_ACCENT_HI : C_ACCENT;
        double in = px96(2), rad = px96(5);
        Scene sc = { C_BG, 0, { { 0 } } };
        if (keys) {
            scene_add(&sc, r.left, r.top, r.right, r.bottom, rad + in, C_TEXT);
            scene_add(&sc, r.left + in / 2, r.top + in / 2, r.right - in / 2, r.bottom - in / 2, rad + in / 2, C_BG);
        }
        scene_add(&sc, r.left + in, r.top + in, r.right - in, r.bottom - in, rad, c);
        aa_render(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, scene_shade, &sc);
        WCHAR t[64];
        GetWindowTextW(b, t, ARRAYSIZE(t));
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    if (id == IDC_UPDATE) { /* a link: accent text, underlined under the mouse */
        WCHAR t[96];
        GetWindowTextW(b, t, ARRAYSIZE(t));
        FillRect(dc, &r, g_ui.brBg);
        int hot = (cd->uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
        SetTextColor(dc, (cd->uItemState & CDIS_SELECTED) ? C_ACCENT_LO : C_ACCENT);
        RECT tr = r, m = r;
        DrawTextW(dc, t, -1, &m, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(dc, t, -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
        int w = m.right - m.left, y = (r.top + r.bottom + (m.bottom - m.top)) / 2;
        if (hot || keys) {
            RECT u = { r.right - w, y, r.right, y + 1 };
            HBRUSH br = CreateSolidBrush(C_ACCENT);
            FillRect(dc, &u, br);
            DeleteObject(br);
        }
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    if (id == IDC_GITHUB) { /* GitHub's mark: grey; under the mouse darker, on a light grey square */
        UINT s = cd->uItemState;
        int size = px96(16), hot = (s & (CDIS_HOT | CDIS_SELECTED)) != 0, down = (s & CDIS_SELECTED) != 0;
        double in = px96(2), rad = px96(4);
        MarkCtx m = { { C_BG, 0, { { 0 } } }, r.left + (r.right - r.left - size) / 2, r.top + (r.bottom - r.top - size) / 2,
                      size / 1600.0, hot && !down ? C_TEXT : C_TEXT2 };
        if (keys) {
            scene_add(&m.bg, r.left, r.top, r.right, r.bottom, rad + in, C_TEXT);
            scene_add(&m.bg, r.left + in / 2, r.top + in / 2, r.right - in / 2, r.bottom - in / 2, rad + in / 2, C_BG);
        }
        if (hot) scene_add(&m.bg, r.left + in, r.top + in, r.right - in, r.bottom - in, rad, down ? RGB(236, 236, 236) : RGB(229, 229, 229));
        aa_render(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mark_shade, &m);
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    if (!card && !is_row_switch(id)) { SelectObject(dc, oldFont); return CDRF_DODEFAULT; }
    FillRect(dc, &r, g_ui.brCard);
    int sw = px96(40), sh = px96(20), sx = r.right - sw - px96(6), sy = r.top + (r.bottom - r.top - sh) / 2;
    int on = SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED;
    draw_switch(dc, sx, sy, sw, sh, on, en, (cd->uItemState & CDIS_HOT) != 0, keys, C_CARD);
    if (!card) { /* row: its text on the left, a grey second line for Task Manager blocking */
        WCHAR t[160];
        GetWindowTextW(b, t, ARRAYSIZE(t));
        RECT tr = { r.left, r.top, sx - px96(10), r.bottom };
        SetTextColor(dc, en ? C_TEXT : C_DISABLED);
        if (id != IDC_STRICT) {
            DrawTextW(dc, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else {
            TEXTMETRICW tm;
            GetTextMetricsW(dc, &tm);
            tr.top += (r.bottom - r.top - 2 * tm.tmHeight) / 2;
            tr.bottom = tr.top + tm.tmHeight;
            DrawTextW(dc, t, -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            OffsetRect(&tr, 0, tm.tmHeight);
            SetTextColor(dc, C_TEXT2);
            DrawTextW(dc, T(S_STRICT_HINT), -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    SelectObject(dc, oldFont);
    return CDRF_SKIPDEFAULT;
}

/* Language list: flag + name (items are added in LANG_* order). */
static void ui_draw_lang_item(const DRAWITEMSTRUCT *di)
{
    if (!g_ui.brCard) return;
    int sel = (di->itemState & ODS_SELECTED) != 0;
    COLORREF bg = GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW);
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(di->hDC, &di->rcItem, br);
    DeleteObject(br);
    if (di->itemID == (UINT)-1 || di->itemID >= LANG_COUNT) return;
    int h = di->rcItem.bottom - di->rcItem.top, fh = h - px96(6);
    if (fh < 8) fh = 8;
    int x = di->rcItem.left + px96(4), y = di->rcItem.top + (h - fh) / 2;
    draw_flag(di->hDC, x, y, fh, (int)di->itemID, bg);
    WCHAR t[32] = L"";
    if (SendMessageW(di->hwndItem, CB_GETLBTEXTLEN, di->itemID, 0) < (LRESULT)ARRAYSIZE(t))
        SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)t);
    HGDIOBJ oldFont = SelectObject(di->hDC, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    RECT tr = di->rcItem;
    tr.left = x + fh * 3 / 2 + px96(8);
    DrawTextW(di->hDC, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(di->hDC, oldFont);
}

static void dlg_status(HWND d)
{
    WCHAR s[STATUS_CCH], cur[STATUS_CCH];
    fmt_status(s, TRUE);
    GetDlgItemTextW(d, IDC_STATUS, cur, ARRAYSIZE(cur));
    if (lstrcmpW(s, cur)) SetDlgItemTextW(d, IDC_STATUS, s);
    /* Next refresh exactly when the countdown shown changes its second, the same moment the
     * warning and the break screen change theirs, so they never disagree. */
    int t;
    ULONGLONG at = 0, now = now_ms();
    if (g_state == ST_BREAK) at = g_breakEnd;
    else if (g_pauseFlags || g_gamingNow || !next_break(&t, &at)) at = 0;
#ifdef PAUSERICK_TEST_TASK
    if (at && g_state != ST_BREAK) timing_log(L"settings", at);
#endif
    SetCoalescableTimer(d, DLG_TIMER_STATUS, at > now ? ms_to_next_second(at, now) : 1000, NULL, TIMERV_NO_COALESCING);
    if (g_ui.brBg) { /* the dot and the progress bar */
        RECT a = ui_dot_rect(d), b = ui_bar_rect(d), u;
        UnionRect(&u, &a, &b);
        InvalidateRect(d, &u, FALSE);
    }
}

/* The header link: "Check for updates", what the last check said, or "Install version X". */
static void dlg_update_link(HWND d)
{
    WCHAR t[96];
    if (g_updBusy)                lstrcpyW(t, T(S_UPD_CHECKING));
    else if (g_upd.version[0])    wsprintfW(t, T(S_UPD_GET_FMT), g_upd.version);
    else if (g_updLast == UPD_UP_TO_DATE) lstrcpyW(t, T(S_UPD_LATEST));
    else if (g_updLast == UPD_ERR_NET || g_updLast == UPD_ERR_DATA) lstrcpyW(t, T(S_UPD_CHECK_FAILED));
    else                          lstrcpyW(t, T(S_UPD_CHECK));
    SetDlgItemTextW(d, IDC_UPDATE, t);
    InvalidateRect(GetDlgItem(d, IDC_UPDATE), NULL, FALSE);
}

static void dlg_texts(HWND d)
{
    SetWindowTextW(d, T(S_DLG_TITLE));
    for (int i = 0; i < (int)ARRAYSIZE(DLG_TEXTS); i++)
        SetDlgItemTextW(d, DLG_TEXTS[i].ctl, T(DLG_TEXTS[i].str));
    for (int i = 0; i < NSLIDERS; i++) slider_label(d, i); /* "Off" is translated */
    int resume = (int)SendDlgItemMessageW(d, IDC_RESUME, CB_GETCURSEL, 0, 0);
    if (resume < 0) resume = g_cfg.idleContinue;
    SendDlgItemMessageW(d, IDC_RESUME, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(d, IDC_RESUME, CB_ADDSTRING, 0, (LPARAM)T(S_RESUME_RESTART));  /* index 0 */
    SendDlgItemMessageW(d, IDC_RESUME, CB_ADDSTRING, 0, (LPARAM)T(S_RESUME_CONTINUE)); /* index 1 */
    SendDlgItemMessageW(d, IDC_RESUME, CB_SETCURSEL, (WPARAM)resume, 0);
    SendDlgItemMessageW(d, IDC_GAMES, EM_SETCUEBANNER, FALSE, (LPARAM)T(S_GAMES_CUE));
    dlg_status(d);
    dlg_update_link(d);
    g_ui.dirty = 1; /* card titles are painted */
    RedrawWindow(d, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static void dlg_sync_enabled(HWND d)
{
    static const int mini[] = { IDC_LBL_MINI_EVERY, IDC_MINI_EVERY, IDC_VAL_MINI_EVERY,
                                IDC_LBL_MINI_DUR, IDC_MINI_DUR, IDC_VAL_MINI_DUR };
    static const int lng[]  = { IDC_LBL_LONG_EVERY, IDC_LONG_EVERY, IDC_VAL_LONG_EVERY,
                                IDC_LBL_LONG_DUR, IDC_LONG_DUR, IDC_VAL_LONG_DUR };
    BOOL m = IsDlgButtonChecked(d, IDC_MINI_ENABLE) == BST_CHECKED;
    BOOL l = IsDlgButtonChecked(d, IDC_LONG_ENABLE) == BST_CHECKED;
    for (int i = 0; i < (int)ARRAYSIZE(mini); i++) {
        EnableWindow(GetDlgItem(d, mini[i]), m);
        EnableWindow(GetDlgItem(d, lng[i]), l);
    }
    BOOL g = IsDlgButtonChecked(d, IDC_GAMING) == BST_CHECKED;
    EnableWindow(GetDlgItem(d, IDC_LBL_GAMES), g);
    EnableWindow(GetDlgItem(d, IDC_GAMES), g);
    EnableWindow(GetDlgItem(d, IDC_PICK_GAMES), g);
    static const int idle[] = { IDC_IDLE, IDC_VAL_IDLE, IDC_LBL_RESUME, IDC_RESUME };
    BOOL ip = IsDlgButtonChecked(d, IDC_IDLE_ENABLE) == BST_CHECKED;
    for (int i = 0; i < (int)ARRAYSIZE(idle); i++) EnableWindow(GetDlgItem(d, idle[i]), ip);
}

/* Show the real startup state (also used to undo a click that failed). */
static void dlg_load_startup(HWND d)
{
    CheckDlgButton(d, IDC_STRICT, strict_registered() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_AUTOSTART, startup_enabled() ? BST_CHECKED : BST_UNCHECKED);
}

/* Break settings: sliders and simple checkboxes. */
static void dlg_save_values(HWND d)
{
    KillTimer(d, DLG_TIMER_SAVE);
    g_dlgDirty = 0;
    Settings n = g_cfg;
    n.miniEnabled = IsDlgButtonChecked(d, IDC_MINI_ENABLE) == BST_CHECKED;
    n.longEnabled = IsDlgButtonChecked(d, IDC_LONG_ENABLE) == BST_CHECKED;
    n.sound       = IsDlgButtonChecked(d, IDC_SOUND) == BST_CHECKED;
    n.idlePause   = IsDlgButtonChecked(d, IDC_IDLE_ENABLE) == BST_CHECKED;
    n.idleContinue = SendDlgItemMessageW(d, IDC_RESUME, CB_GETCURSEL, 0, 0) == 1;
    n.gaming      = IsDlgButtonChecked(d, IDC_GAMING) == BST_CHECKED;
    n.autoUpdate  = IsDlgButtonChecked(d, IDC_AUTOUPDATE) == BST_CHECKED;
    GetDlgItemTextW(d, IDC_GAMES, n.games, ARRAYSIZE(n.games));
    for (int i = 0; i < NSLIDERS; i++) *cfg_field(&n, &SLIDERS[i]) = g_dlgVal[i];
    g_fgHwnd = NULL; /* the game list may have changed: classify the foreground app again */

    int timingChanged = n.miniEnabled != g_cfg.miniEnabled || n.miniEveryMin != g_cfg.miniEveryMin ||
                        n.longEnabled != g_cfg.longEnabled || n.longEveryMin != g_cfg.longEveryMin;
    log_settings_diff(&g_cfg, &n);
    if (n.autoUpdate && !g_cfg.autoUpdate) SetCoalescableTimer(g_main, TIMER_UPDATE, 3000, NULL, 1000); /* look soon */
    g_cfg = n;
    settings_save();
    if (timingChanged) g_miniRef = g_longRef = now_ms();
    if (g_state != ST_BREAK) tick();
    dlg_status(d);
}

/* ---- "Choose app...": pick games among the apps that are open right now ---- */

#define MAX_APPS      40
#define IDM_APP_FIRST 3000

typedef struct { WCHAR name[64], title[48], path[MAX_PATH]; HBITMAP icon, menuBmp; } AppEntry;
typedef struct { AppEntry e[MAX_APPS]; int n, winLen; WCHAR winDir[MAX_PATH]; } AppList;

/* Apps with a normal visible window. Same anti-cheat rules as gaming mode: only
 * window-manager information and the handle-free exe path query. */
static BOOL CALLBACK collect_app(HWND w, LPARAM lp)
{
    AppList *l = (AppList *)lp;
    if (l->n >= MAX_APPS) return FALSE;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;
    BOOL cloaked = FALSE; /* suspended Store apps, other virtual desktops */
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
    WCHAR title[48];
    if (GetWindowTextW(w, title, ARRAYSIZE(title)) <= 0) return TRUE; /* no message is sent to the app */
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    WCHAR path[MAX_PATH * 2];
    if (!process_image_path(pid, path, ARRAYSIZE(path))) return TRUE;
    if (l->winLen && CompareStringOrdinal(path, l->winLen, l->winDir, l->winLen, TRUE) == CSTR_EQUAL &&
        path[l->winLen] == L'\\')
        return TRUE; /* Windows' own programs (Explorer, Settings...) */
    const WCHAR *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    for (int i = 0; i < l->n; i++)
        if (CompareStringOrdinal(l->e[i].name, -1, name, -1, TRUE) == CSTR_EQUAL) return TRUE;
    lstrcpynW(l->e[l->n].name, name, ARRAYSIZE(l->e[0].name));
    lstrcpynW(l->e[l->n].title, title, ARRAYSIZE(l->e[0].title));
    lstrcpynW(l->e[l->n].path, path, ARRAYSIZE(l->e[0].path));
    l->e[l->n].icon = l->e[l->n].menuBmp = NULL;
    l->n++;
    return TRUE;
}

/* An app's icon as a 32-bit premultiplied-alpha bitmap (what menus need), cx pixels.
 * Read from the exe file on disk: nothing is sent to the running app. NULL if none. */
static HBITMAP app_icon_bitmap(const WCHAR *exe, int cx)
{
    HICON ic = NULL;
    if (SHDefExtractIconW(exe, 0, 0, &ic, NULL, (UINT)cx) != S_OK || !ic) return NULL;
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cx;
    bi.bmiHeader.biHeight = -cx; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        DrawIconEx(dc, 0, 0, ic, cx, cx, 0, NULL, DI_NORMAL);
        GdiFlush();
        DWORD *px = (DWORD *)bits;
        int n = cx * cx, alpha = 0;
        for (int i = 0; i < n && !alpha; i++) alpha = (px[i] & 0xFF000000u) != 0;
        if (!alpha) { /* old-style icon without an alpha channel: take transparency from its mask */
            void *mbits = NULL;
            HBITMAP mbmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &mbits, NULL, 0);
            HDC mdc = CreateCompatibleDC(NULL);
            if (mbmp && mbits && mdc) {
                HGDIOBJ mold = SelectObject(mdc, mbmp);
                DrawIconEx(mdc, 0, 0, ic, cx, cx, 0, NULL, DI_MASK);
                GdiFlush();
                const DWORD *mp = (const DWORD *)mbits;
                for (int i = 0; i < n; i++) px[i] = (mp[i] & 0x00FFFFFFu) ? 0 : (px[i] | 0xFF000000u);
                SelectObject(mdc, mold);
            }
            if (mdc) DeleteDC(mdc);
            if (mbmp) DeleteObject(mbmp);
        }
        SelectObject(dc, old);
    } else if (bmp) {
        DeleteObject(bmp);
        bmp = NULL;
    }
    if (dc) DeleteDC(dc);
    DestroyIcon(ic);
    return bmp;
}

static void apps_load_icons(AppList *l, int cx)
{
    for (int i = 0; i < l->n; i++) l->e[i].icon = app_icon_bitmap(l->e[i].path, cx);
}

static void apps_free_icons(AppList *l)
{
    for (int i = 0; i < l->n; i++) {
        if (l->e[i].icon) { DeleteObject(l->e[i].icon); l->e[i].icon = NULL; }
        if (l->e[i].menuBmp) { DeleteObject(l->e[i].menuBmp); l->e[i].menuBmp = NULL; }
    }
}

static int in_round_box(double x, double y, double x0, double y0, double x1, double y1, double r)
{
    if (x < x0 || x > x1 || y < y0 || y > y1) return 0;
    double cx = x < x0 + r ? x0 + r : (x > x1 - r ? x1 - r : x);
    double cy = y < y0 + r ? y0 + r : (y > y1 - r ? y1 - r : y);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static double seg_dist2(double px, double py, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double ex = ax + t * dx - px, ey = ay + t * dy - py;
    return ex * ex + ey * ey;
}

/* Menu item bitmap: [checkbox] [app icon], 32-bit premultiplied alpha, cx high.
 * In the list: a filled teal box with a white tick; not in the list: an empty grey
 * box. Drawn at the far left, so all ticks line up in one column. */
static HBITMAP app_menu_bitmap(HBITMAP icon, int cx, int checked)
{
    int gap = cx / 3 > 3 ? cx / 3 : 3, w = cx + gap + cx;
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -cx; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return NULL; }
    DWORD *px = (DWORD *)bits;
    ZeroMemory(px, (SIZE_T)w * cx * 4);

    const double s = cx, inset = 0.5, rad = s * 0.22, ring = s / 14.0 < 1.0 ? 1.0 : s / 14.0;
    const double half = (s * 0.14 < 1.7 ? 1.7 : s * 0.14) / 2.0; /* tick stroke half-width */
    for (int y = 0; y < cx; y++)
        for (int x = 0; x < cx; x++) {
            int box = 0, inner = 0, tick = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double fx = x + (sx + 0.5) / 4.0, fy = y + (sy + 0.5) / 4.0;
                    if (!in_round_box(fx, fy, inset, inset, s - inset, s - inset, rad)) continue;
                    box++;
                    if (in_round_box(fx, fy, inset + ring, inset + ring, s - inset - ring, s - inset - ring, rad - ring))
                        inner++;
                    if (checked) {
                        double d1 = seg_dist2(fx, fy, s * 0.25, s * 0.52, s * 0.43, s * 0.70);
                        double d2 = seg_dist2(fx, fy, s * 0.43, s * 0.70, s * 0.76, s * 0.32);
                        if ((d1 < d2 ? d1 : d2) <= half * half) tick++;
                    }
                }
            double a, r, g, b;
            if (checked) { /* teal #0F766E, white tick */
                if (!box) continue;
                double wt = (double)tick / box;
                a = box / 16.0;
                r = 0x0F + (255 - 0x0F) * wt; g = 0x76 + (255 - 0x76) * wt; b = 0x6E + (255 - 0x6E) * wt;
            } else {       /* grey outline #8C8C8C */
                a = (box - inner) / 16.0;
                r = g = b = 0x8C;
            }
            px[y * w + x] = ((DWORD)(a * 255 + 0.5) << 24) | ((DWORD)(r * a + 0.5) << 16) |
                            ((DWORD)(g * a + 0.5) << 8) | (DWORD)(b * a + 0.5);
        }

    DIBSECTION ds;
    if (icon && GetObjectW(icon, sizeof ds, &ds) == sizeof ds && ds.dsBm.bmBits &&
        ds.dsBm.bmWidth == cx && ds.dsBm.bmHeight == cx && ds.dsBm.bmBitsPixel == 32) {
        const DWORD *src = (const DWORD *)ds.dsBm.bmBits; /* top-down, like ours */
        for (int y = 0; y < cx; y++) CopyMemory(px + y * w + cx + gap, src + y * cx, (SIZE_T)cx * 4);
    }
    return bmp;
}

static void collect_apps(AppList *l)
{
    l->n = 0;
    UINT wl = GetWindowsDirectoryW(l->winDir, ARRAYSIZE(l->winDir));
    l->winLen = wl < ARRAYSIZE(l->winDir) ? (int)wl : 0;
    EnumWindows(collect_app, (LPARAM)l);
    for (int i = 1; i < l->n; i++) /* sort by name */
        for (int j = i; j > 0 && CompareStringOrdinal(l->e[j - 1].name, -1, l->e[j].name, -1, TRUE) == CSTR_GREATER_THAN; j--) {
            AppEntry t = l->e[j];
            l->e[j] = l->e[j - 1];
            l->e[j - 1] = t;
        }
}

static void list_append(WCHAR *out, int cap, const WCHAR *tok)
{
    int a = lstrlenW(out), b = lstrlenW(tok);
    if (a + b + 3 >= cap) return; /* full: never overflow */
    if (a) lstrcatW(out, L"; ");
    lstrcatW(out, tok);
}

/* Adds the exe name to the games field, or removes it if it is already there. */
static void games_toggle(HWND d, const WCHAR *name)
{
    WCHAR cur[ARRAYSIZE(g_cfg.games)], out[ARRAYSIZE(g_cfg.games)], tok[MAX_PATH];
    const WCHAR *p = cur;
    int found = 0;
    GetDlgItemTextW(d, IDC_GAMES, cur, ARRAYSIZE(cur));
    out[0] = 0;
    while (list_next(&p, tok, ARRAYSIZE(tok))) {
        if (entry_is_name(tok, name)) { found = 1; continue; }
        list_append(out, ARRAYSIZE(out), tok);
    }
    if (!found) list_append(out, ARRAYSIZE(out), name);
    g_dlgLoading = 1;
    SetDlgItemTextW(d, IDC_GAMES, out);
    g_dlgLoading = 0;
    dlg_save_values(d);
}

/* Popup menu for the app list: [checkbox][icon] exe name, window title.
 * The bitmaps stay alive until apps_free_icons(), after the menu is destroyed. */
static HMENU build_app_menu(AppList *l, const WCHAR *cur, int cx)
{
    HMENU m = CreatePopupMenu();
    if (!m) return NULL;
    if (!l->n) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(S_NO_APPS));
    for (int i = 0; i < l->n; i++) {
        WCHAR item[2 * 64 + 2 * 48 + 4], *q = item; /* worst case: every char is '&' */
        const WCHAR *s;
        for (s = l->e[i].name; *s; s++) { if (*s == L'&') *q++ = L'&'; *q++ = *s; }
        *q++ = L'\t'; /* title in the right-hand column */
        for (s = l->e[i].title; *s; s++) { if (*s == L'&') *q++ = L'&'; *q++ = *s; }
        *q = 0;
        if (l->e[i].menuBmp) DeleteObject(l->e[i].menuBmp);
        l->e[i].menuBmp = app_menu_bitmap(l->e[i].icon, cx, list_has_name(cur, l->e[i].name));
        MENUITEMINFOW mii = { sizeof mii };
        mii.fMask = MIIM_ID | MIIM_STRING | (l->e[i].menuBmp ? MIIM_BITMAP : 0);
        mii.wID = (UINT)(IDM_APP_FIRST + i);
        mii.dwTypeData = item;
        mii.hbmpItem = l->e[i].menuBmp;
        InsertMenuItemW(m, (UINT)i, TRUE, &mii);
    }
    return m;
}

/* Menu of open apps under the button; click toggles an app in the list. */
static void games_pick(HWND d)
{
    AppList *l = (AppList *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *l); /* freed below */
    if (!l) return;
    collect_apps(l);
    int cx = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(d));
    apps_load_icons(l, cx);
    WCHAR cur[ARRAYSIZE(g_cfg.games)];
    GetDlgItemTextW(d, IDC_GAMES, cur, ARRAYSIZE(cur));
    HMENU m = build_app_menu(l, cur, cx);
    if (m) {
        RECT r;
        GetWindowRect(GetDlgItem(d, IDC_PICK_GAMES), &r);
        int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_TOPALIGN,
                                      r.right, r.bottom, 0, d, NULL);
        DestroyMenu(m);
        if (cmd >= IDM_APP_FIRST && cmd < IDM_APP_FIRST + l->n) games_toggle(d, l->e[cmd - IDM_APP_FIRST].name);
    }
    apps_free_icons(l);
    HeapFree(GetProcessHeap(), 0, l);
}

/* WM_HSCROLL from a slider: the value label follows at once, saving happens
 * when the drag ends (or shortly after keyboard / mouse wheel steps). */
static void slider_moved(HWND d, HWND t, int code)
{
    for (int i = 0; i < NSLIDERS; i++) {
        if (GetDlgItem(d, SLIDERS[i].ctl) != t) continue;
        int pos = clampi((int)SendMessageW(t, TBM_GETPOS, 0, 0), 0, SLIDERS[i].n - 1);
        int v = SLIDERS[i].vals[pos];
        if (v != g_dlgVal[i]) {
            g_dlgVal[i] = v;
            slider_label(d, i);
            g_dlgDirty = 1;
        }
        if (g_dlgDirty) {
            if (code == TB_ENDTRACK) dlg_save_values(d);
            else SetTimer(d, DLG_TIMER_SAVE, DLG_SAVE_DELAY, NULL);
        }
        return;
    }
}

/* Hand over to an elevated instance started through the strict mode task. */
static void handover_to_task(void)
{
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
    tray_remove();
    AllowSetForegroundWindow(ASFW_ANY); /* so the new instance can bring its window up */
    if (task_run()) {
        log_info(L"Handing over to the instance with administrator rights started by the task");
        DestroyWindow(g_main);
        return;
    }
    log_warn(L"Could not start the task: continuing without administrator rights");
    g_mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    tray_add();
}

/* "Start with Windows" and "Strict mode" apply on click; they may need a UAC prompt.
 * Normal mode: Run entry = start with Windows.
 * Strict mode: Pauserick installed in Program Files and an elevated task starting that
 * copy; the task's logon trigger = start with Windows. */
static void dlg_apply_startup(HWND d)
{
    int strict = IsDlgButtonChecked(d, IDC_STRICT) == BST_CHECKED;
    int autoStart = IsDlgButtonChecked(d, IDC_AUTOSTART) == BST_CHECKED;
    int wasStrict = strict_registered();

    if (strict) {
        WCHAR dir[MAX_PATH], inst[MAX_PATH];
        int haveInst = strict_install_path(dir, inst);
        int needTask = !strict_at_install_path() || startup_enabled() != autoStart ||
                       (haveInst && GetFileAttributesW(inst) == INVALID_FILE_ATTRIBUTES);
        if (needTask && !strict_install(autoStart)) {
            MessageBoxW(d, T(wasStrict ? S_ERR_AUTOSTART : S_ERR_STRICT_ON), APP_NAME, MB_OK | MB_ICONWARNING);
            dlg_load_startup(d);
            return;
        }
        WCHAR run[MAX_PATH * 2];
        DWORD sz = sizeof run;
        if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_NAME, RRF_RT_REG_SZ, NULL, run, &sz) == ERROR_SUCCESS)
            autostart_set(0); /* the task replaces the Run entry */
        if (needTask && (!g_elevated || !haveInst || lstrcmpiW(g_exe, inst))) {
            /* continue in the installed, elevated instance, which reopens this window */
            WritePrivateProfileStringW(L"State", L"OpenSettings", L"1", g_ini);
            DestroyWindow(d);
            handover_to_task();
        }
        return;
    }
    if (wasStrict && !task_delete()) {
        MessageBoxW(d, T(S_ERR_STRICT_OFF), APP_NAME, MB_OK | MB_ICONWARNING);
        dlg_load_startup(d);
        return;
    }
    if (autoStart != autostart_get()) autostart_set(autoStart);
}

static INT_PTR CALLBACK settings_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (m) {
    case WM_MEASUREITEM: { /* language list, asked while the dialog is being created */
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        if (mi->CtlID != IDC_LANG) break;
        UINT dpi = GetDpiForWindow(d);
        mi->itemHeight = (UINT)MulDiv(22, dpi ? (int)dpi : 96, 96);
        return TRUE;
    }
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlID != IDC_LANG) break;
        ui_draw_lang_item((DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code != NM_CUSTOMDRAW) break;
        SetWindowLongPtrW(d, DWLP_MSGRESULT, ui_custom_draw(d, (NMCUSTOMDRAW *)lp));
        return TRUE;
    case WM_SETCURSOR: /* the header links show the hand */
        if (LOWORD(lp) == HTCLIENT && ((HWND)wp == GetDlgItem(d, IDC_GITHUB) || (HWND)wp == GetDlgItem(d, IDC_UPDATE))) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            SetWindowLongPtrW(d, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (!g_ui.brCard) break;
        return (INT_PTR)ui_ctlcolor(d, (HDC)wp, (HWND)lp);
    case WM_CTLCOLORDLG:
        if (!g_ui.brBg) break;
        return (INT_PTR)g_ui.brBg;
    case WM_ERASEBKGND: /* our own paint does it all; a child asking for its background gets it */
        if (!g_ui.inPaint && g_ui.brBg) {
            RECT r;
            if (GetClipBox((HDC)wp, &r) != ERROR) ui_paint(d, (HDC)wp, &r);
        }
        SetWindowLongPtrW(d, DWLP_MSGRESULT, 1);
        return TRUE;
    case WM_PRINTCLIENT: {
        RECT r;
        GetClientRect(d, &r);
        ui_paint(d, (HDC)wp, &r);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        g_ui.inPaint = 1;
        HDC dc = BeginPaint(d, &ps);
        ui_paint(d, dc, &ps.rcPaint);
        EndPaint(d, &ps);
        g_ui.inPaint = 0;
        return TRUE;
    }
    case WM_DPICHANGED: /* the dialog manager rescales the controls, then we redo fonts and painting */
        PostMessageW(d, WM_APP_DLG_DPI, 0, 0);
        return FALSE;
    case WM_APP_DLG_DPI:
        ui_fonts(d);
        RedrawWindow(d, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return TRUE;
    case WM_INITDIALOG: {
        ui_init(d);
        HICON big = NULL, small = NULL;
        LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big);
        LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &small);
        SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)big);
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)small);
        SendDlgItemMessageW(d, IDC_LANG, CB_ADDSTRING, 0, (LPARAM)L"English");
        SendDlgItemMessageW(d, IDC_LANG, CB_ADDSTRING, 0, (LPARAM)L"Italiano");
        SendDlgItemMessageW(d, IDC_LANG, CB_SETCURSEL, (WPARAM)g_cfg.lang, 0);
        CheckDlgButton(d, IDC_MINI_ENABLE, g_cfg.miniEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_LONG_ENABLE, g_cfg.longEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_SOUND, g_cfg.sound ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_IDLE_ENABLE, g_cfg.idlePause ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_GAMING, g_cfg.gaming ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_AUTOUPDATE, g_cfg.autoUpdate ? BST_CHECKED : BST_UNCHECKED);
        g_dlgLoading = 1;
        SendDlgItemMessageW(d, IDC_GAMES, EM_LIMITTEXT, ARRAYSIZE(g_cfg.games) - 1, 0);
        SetDlgItemTextW(d, IDC_GAMES, g_cfg.games);
        g_dlgLoading = 0;
        dlg_load_startup(d); /* Task Manager blocking is off until the user ticks it */
        sliders_init(d);
        g_dlgDirty = 0;
        dlg_texts(d);
        dlg_sync_enabled(d);
        dlg_status(d); /* also starts its timer */
        return TRUE;
    }
    case WM_TIMER:
        if (wp == DLG_TIMER_SAVE) dlg_save_values(d);
        else dlg_status(d);
        return TRUE;
    case WM_HSCROLL:
        if (lp) slider_moved(d, (HWND)lp, LOWORD(wp));
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDOK: /* Enter: save right away, keep the window open */
            if (g_dlgDirty) dlg_save_values(d);
            return TRUE;
        case IDCANCEL: /* Close button, Esc, title bar X; WM_DESTROY saves pending edits */
            DestroyWindow(d);
            return TRUE;
        case IDC_MINI_ENABLE:
        case IDC_LONG_ENABLE:
        case IDC_IDLE_ENABLE:
            if (code == BN_CLICKED) { dlg_sync_enabled(d); dlg_save_values(d); }
            return TRUE;
        case IDC_RESUME:
            if (code == CBN_SELCHANGE) dlg_save_values(d);
            return TRUE;
        case IDC_SOUND:
        case IDC_AUTOUPDATE:
            if (code == BN_CLICKED) dlg_save_values(d);
            return TRUE;
        case IDC_UPDATE: /* check now, or offer the version already found */
            if (code != BN_CLICKED) return TRUE;
            if (g_upd.version[0]) PostMessageW(g_main, WM_APP_UPDATE_PROMPT, 0, 0);
            else update_check_start(1);
            return TRUE;
        case IDC_GITHUB:
            if (code == BN_CLICKED) open_url(PROJECT_URL);
            return TRUE;
        case IDC_GAMING:
            if (code == BN_CLICKED) { dlg_sync_enabled(d); dlg_save_values(d); }
            return TRUE;
        case IDC_PICK_GAMES:
            if (code == BN_CLICKED) games_pick(d);
            return TRUE;
        case IDC_GAMES: /* saved shortly after typing stops, or when leaving the field */
            if (code == EN_CHANGE && !g_dlgLoading) {
                g_dlgDirty = 1;
                SetTimer(d, DLG_TIMER_SAVE, 800, NULL);
            } else if (code == EN_KILLFOCUS && g_dlgDirty) {
                dlg_save_values(d);
            }
            return TRUE;
        case IDC_AUTOSTART:
        case IDC_STRICT:
            if (code == BN_CLICKED) dlg_apply_startup(d); /* may destroy the dialog */
            return TRUE;
        case IDC_LANG:
            if (code == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessageW(d, IDC_LANG, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < LANG_COUNT && sel != g_cfg.lang) {
                    log_info(L"Setting changed: language -> %s", sel == LANG_IT ? L"Italiano" : L"English");
                    g_cfg.lang = sel;
                    settings_save();
                    dlg_texts(d);
                }
            }
            return TRUE;
        }
        break;
    }
    case WM_DESTROY: {
        if (g_dlgDirty) dlg_save_values(d);
        KillTimer(d, DLG_TIMER_STATUS);
        KillTimer(d, DLG_TIMER_SAVE);
        HICON big = (HICON)SendMessageW(d, WM_GETICON, ICON_BIG, 0);
        HICON small = (HICON)SendMessageW(d, WM_GETICON, ICON_SMALL, 0);
        if (big) DestroyIcon(big);
        if (small) DestroyIcon(small);
        ui_free();
        g_settings = NULL;
        trim_memory();
        return TRUE;
    }
    }
    return FALSE;
}

static void show_settings(void)
{
    if (g_state == ST_BREAK) return;
    if (!g_settings)
        g_settings = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, settings_proc, 0);
#ifdef PAUSERICK_TEST_TASK
    if (g_timingLog) return; /* timing test: the window works but stays invisible */
    if (g_offscreen && g_settings) { /* screenshots: drawn off the screen, nothing pops up */
        SetWindowPos(g_settings, NULL, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(g_settings, SW_SHOWNOACTIVATE);
        return;
    }
#endif
    if (g_settings) {
        ShowWindow(g_settings, SW_SHOWNORMAL);
        SetForegroundWindow(g_settings);
    }
}

/* ------------------------------------------------- updates: asking, installing */

static HRESULT CALLBACK update_dlg_cb(HWND h, UINT n, WPARAM wp, LPARAM lp, LONG_PTR ref)
{
    (void)wp; (void)ref;
    if (n == TDN_CREATED) /* above other windows so it is seen, but without taking the keyboard */
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    else if (n == TDN_HYPERLINK_CLICKED && lp)
        open_url((LPCWSTR)lp);
    return S_OK;
}

static DWORD WINAPI update_install_thread(LPVOID unused)
{
    (void)unused;
    WCHAR probe[MAX_PATH];
    exe_sibling(probe, g_exe, L".update.tmp");
    /* can this process write next to the exe? Otherwise the elevated helper does the job */
    HANDLE f = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    int r;
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        r = update_apply(g_upd.exeUrl, g_upd.sigUrl, g_upd.version);
    } else if (wcschr(g_upd.exeUrl, L'"') || wcschr(g_upd.sigUrl, L'"')) {
        r = UPD_ERR_DATA;
    } else {
        WCHAR params[1400];
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); /* for the UAC prompt */
        wsprintfW(params, L"--apply-update \"%s\" \"%s\" %s --config \"%s\"", g_upd.exeUrl, g_upd.sigUrl, g_upd.version, g_ini);
        DWORD rc = run_self_elevated(params, 180000);
        CoUninitialize();
        r = rc == (DWORD)-1 ? UPD_ERR_RIGHTS : (int)rc;
        if (rc == (DWORD)-1) log_warn(L"Update %s not installed: administrator rights were not granted", g_upd.version);
    }
    PostMessageW(g_main, WM_APP_UPDATE_DONE, (WPARAM)r, 0);
    return 0;
}

/* The new version is offered: install now, not now (asked again tomorrow), skip it. */
static void update_prompt(void)
{
    static int open;
    if (open || !g_upd.version[0] || g_updBusy) return;
    open = 1;
    g_updPrompt = 0;
    WCHAR title[96], text[400], footer[200];
    wsprintfW(title, T(S_UPD_TITLE_FMT), g_upd.version);
    wsprintfW(text, T(S_UPD_TEXT_FMT), APP_VERSION);
    wsprintfW(footer, L"<a href=\"%s\">github.com/%s</a>", UPDATE_PAGE_URL, L"" PAUSERICK_REPO);
    TASKDIALOG_BUTTON buttons[] = { { 100, T(S_UPD_INSTALL) }, { 101, T(S_UPD_LATER) }, { 102, T(S_UPD_SKIP) } };
    TASKDIALOGCONFIG tc = { sizeof tc };
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = title;
    tc.pszContent = text;
    tc.pszFooter = footer;
    if (g_upd.notes[0]) {
        tc.pszExpandedInformation = g_upd.notes;
        tc.pszCollapsedControlText = T(S_UPD_NOTES);
        tc.pszExpandedControlText = T(S_UPD_HIDE_NOTES);
    }
    tc.pButtons = buttons;
    tc.cButtons = ARRAYSIZE(buttons);
    tc.nDefaultButton = 100;
    tc.pfCallback = update_dlg_cb;
    int choice = 101;
    log_info(L"Update %s offered", g_upd.version);
    if (FAILED(TaskDialogIndirect(&tc, &choice, NULL, NULL))) choice = 101;
    open = 0;
    if (choice == 100 && !InterlockedCompareExchange(&g_updBusy, 1, 0)) {
        log_info(L"Update %s: installing", g_upd.version);
        HANDLE t = CreateThread(NULL, 0, update_install_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        else InterlockedExchange(&g_updBusy, 0);
    } else if (choice == 102) {
        WritePrivateProfileStringW(L"Update", L"Skip", g_upd.version, g_ini);
        log_info(L"Update %s skipped: waiting for the next version", g_upd.version);
    } else if (choice != 100) {
        log_info(L"Update %s: not now, asking again tomorrow", g_upd.version);
    }
    if (g_settings) dlg_update_link(g_settings);
}

/* The new exe is in place: start it and leave. In strict mode the elevated task starts it. */
static void update_restart(void)
{
    WritePrivateProfileStringW(L"Update", L"From", APP_VERSION, g_ini); /* the new instance logs it */
    if (g_settings) DestroyWindow(g_settings);
    if (g_elevated && strict_matches_exe()) {
        handover_to_task();
        return;
    }
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
    tray_remove();
    WCHAR cmd[MAX_PATH * 2 + 32];
    if (g_customIni) wsprintfW(cmd, L"\"%s\" --config \"%s\"", g_exe, g_ini);
    else             wsprintfW(cmd, L"\"%s\"", g_exe);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(g_exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        log_info(L"Restarting with the new version");
        DestroyWindow(g_main);
        return;
    }
    log_error(L"Could not start the new version (error %lu): it starts next time", GetLastError());
    g_mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    tray_add();
}

static void update_done(int r)
{
    InterlockedExchange(&g_updBusy, 0);
    if (r == UPD_OK) {
        if (g_state == ST_BREAK) g_updRestart = 1; /* never cut a break short: restart after it */
        else update_restart();
        return;
    }
    WCHAR msg[600];
    wsprintfW(msg, T(S_UPD_FAILED_FMT), T(r == UPD_ERR_SIG ? S_UPD_E_SIG : r == UPD_ERR_FILE ? S_UPD_E_FILE :
                                          r == UPD_ERR_RIGHTS ? S_UPD_E_RIGHTS : S_UPD_E_NET));
    MessageBoxW(g_settings, msg, APP_NAME, MB_OK | MB_ICONWARNING);
    if (g_settings) dlg_update_link(g_settings);
}

/* ----------------------------------------------------------- main window */

static void cleanup(void)
{
    if (g_settings) DestroyWindow(g_settings); /* saves an edit still pending */
    if (g_state == ST_BREAK) {
        overlays_destroy();
        InterlockedExchange64(&g_blockUntil, 0);
        hooks_stop();
        SetThreadExecutionState(ES_CONTINUOUS);
        g_state = ST_IDLE;
    }
    taskmgr_unlock();
    warn_close();
    tray_remove();
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TIMER:
        if (wp == TIMER_TICK) tick();
        else if (wp == TIMER_UPDATE) update_timer();
        else if (wp == TIMER_REBUILD) {
            KillTimer(h, TIMER_REBUILD);
            if (g_state == ST_BREAK) {
                overlays_destroy();
                overlays_create();
                if (!g_nov) { /* no monitor left: end the break */
                    log_warn(L"Monitors changed during the break and none is left: break ended, input released");
                    break_teardown();
                    tick();
                } else {
                    WCHAR screens[200];
                    describe_screens(screens, ARRAYSIZE(screens));
                    log_info(L"Monitors changed during the break: break screens rebuilt on %d screen(s) [%s]",
                             g_nov, screens);
                }
            }
        }
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_MOUSEMOVE:   tray_update_tip(); break;
        case WM_LBUTTONUP:   show_settings(); break;
        case WM_RBUTTONUP:   tray_menu(); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TAKE_MINI: start_break(BT_MINI, L"from the tray menu"); break;
        case IDM_TAKE_LONG: start_break(BT_LONG, L"from the tray menu"); break;
        case IDM_SETTINGS:  show_settings(); break;
        case IDM_UPDATE:    update_prompt(); break;
        case IDM_OPEN_LOG:  /* the user's default text editor */
            if ((INT_PTR)ShellExecuteW(NULL, L"open", g_logPath, NULL, NULL, SW_SHOWNORMAL) <= 32)
                ShellExecuteW(NULL, L"open", L"notepad.exe", g_logPath, NULL, SW_SHOWNORMAL);
            break;
        case IDM_EXIT:      log_info(L"Exit chosen from the tray menu"); DestroyWindow(h); break;
        }
        return 0;
    case WM_APP_SHOW_SETTINGS:
        show_settings();
        return 0;
    case WM_APP_BREAK_NOW:
        start_break(wp ? BT_LONG : BT_MINI, L"from the command line");
        return 0;
    case WM_APP_UPDATE:
        update_checked((UpdInfo *)lp);
        return 0;
    case WM_APP_UPDATE_PROMPT:
        if (g_state != ST_BREAK) update_prompt();
        else g_updPrompt = 1; /* after the break */
        return 0;
    case WM_APP_UPDATE_DONE:
        update_done((int)wp);
        return 0;
    case WM_DISPLAYCHANGE:
        /* monitors added/removed/resized: rebuild the break screens once things settle */
        if (g_state == ST_BREAK) SetTimer(h, TIMER_REBUILD, 400, NULL);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) pause_begin(PAUSE_LOCK, now_ms());
        else if (wp == WTS_SESSION_UNLOCK) pause_end(PAUSE_LOCK | PAUSE_IDLE); /* unlocking = the user is back */
        return 0;
    case WM_POWERBROADCAST: /* waking up may be automatic: an idle pause ends only with real input */
        if (wp == PBT_APMSUSPEND) pause_begin(PAUSE_SUSPEND, now_ms());
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) pause_end(PAUSE_SUSPEND);
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            log_info(L"Windows is signing out or shutting down");
            cleanup();
        }
        return 0;
    case WM_CLOSE:
        if (g_state != ST_BREAK) {
            log_info(L"Asked to close");
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        WTSUnRegisterSessionNotification(h);
        cleanup();
        log_info(L"Pauserick stopped");
        PostQuitMessage(0);
        return 0;
    default:
        if (m == g_msgTaskbarCreated && m) { /* Explorer restarted */
            g_trayAdded = 0;
            tray_add();
            log_info(L"Taskbar restarted: tray icon added again");
            return 0;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}

#ifdef PAUSERICK_TEST_TASK
#include <psapi.h>
/* Test build only: exercise the real code paths many times with all windows
 * hidden, and report handle / memory growth (leaks) plus the watchdog. */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *e);

static void selftest(const WCHAR *outPath, int cycles, int audioOnly)
{
    static char out[8192];
    int n = 0;
    HANDLE me = GetCurrentProcess();
    WCHAR logPath[MAX_PATH];
    lstrcpyW(logPath, g_logPath); /* logging stays on for the watchdog/crash tests, off for the stress loops */
    g_show = 0;
    if (audioOnly) g_logPath[0] = 0;

    if (audioOnly) { /* the check that runs every 5 s while the user is away */
        PROCESS_MEMORY_COUNTERS_EX m[3];
        DWORD h[3];
        for (int stage = 0, done = 0; stage < 3; stage++) {
            int until = stage == 0 ? 20 : stage == 1 ? cycles / 2 : cycles;
            for (; done < until; done++) audio_playing();
            K32GetProcessMemoryInfo(me, (PPROCESS_MEMORY_COUNTERS)&m[stage], sizeof m[0]);
            GetProcessHandleCount(me, &h[stage]);
        }
        n = wsprintfA(out, "sound check: after 20: %u KB, %u handles | after %d: %u KB, %u handles | after %d: %u KB, %u handles\r\n",
                      (unsigned)(m[0].PrivateUsage / 1024), (unsigned)h[0], cycles / 2, (unsigned)(m[1].PrivateUsage / 1024),
                      (unsigned)h[1], cycles, (unsigned)(m[2].PrivateUsage / 1024), (unsigned)h[2]);
        HANDLE f = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD wr;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, out, (DWORD)n, &wr, NULL); CloseHandle(f); }
        return;
    }

    /* 1. hook decision before / after the deadline (pure function calls, nothing installed) */
    KBDLLHOOKSTRUCT k = { 0 };
    MSLLHOOKSTRUCT ms = { 0 };
    k.vkCode = 'A';
    InterlockedExchange64(&g_blockUntil, (LONGLONG)GetTickCount64() + 60000);
    LRESULT bk = kbd_hook(HC_ACTION, WM_KEYDOWN, (LPARAM)&k), bm = mouse_hook(HC_ACTION, WM_MOUSEMOVE, (LPARAM)&ms);
    InterlockedExchange64(&g_blockUntil, (LONGLONG)GetTickCount64() - 1);
    LRESULT pk = kbd_hook(HC_ACTION, WM_KEYDOWN, (LPARAM)&k), pm = mouse_hook(HC_ACTION, WM_MOUSEMOVE, (LPARAM)&ms);
    n += wsprintfA(out + n, "hooks during break: key=%d mouse=%d (1 = blocked) | after deadline: key=%d mouse=%d (0 = let through)\r\n",
                   (int)bk, (int)bm, (int)pk, (int)pm);

    /* 2. watchdog: the UI thread "freezes" (never calls hooks_stop); the hook thread must end by itself.
     *    The deadline is already past, so the user's input is never blocked by this test. */
    ULONGLONG t0 = GetTickCount64();
    hooks_start();
    DWORD w = g_hookThread ? WaitForSingleObject(g_hookThread, 5000) : WAIT_FAILED;
    n += wsprintfA(out + n, "watchdog: hook thread ended on its own: %s (after %u ms)\r\n",
                   w == WAIT_OBJECT_0 ? "yes" : "NO", (unsigned)(GetTickCount64() - t0));
    hooks_stop();

    /* crash handler on a simulated access violation (nothing really crashes) */
    {
        EXCEPTION_RECORD er = { 0 };
        CONTEXT ctx;
        EXCEPTION_POINTERS ep = { &er, &ctx };
        er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        er.ExceptionAddress = (PVOID)((ULONG_PTR)g_inst + 0x1234);
        crash_handler(&ep);
    }

    /* 3. leak check */
    g_logPath[0] = 0;
    AppList *apps = (AppList *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *apps);
    int mons = 0;
    for (int pass = 0; pass < 2; pass++) {
        int reps = pass == 0 ? 3 : cycles; /* pass 0 warms up lazy one-time allocations */
        PROCESS_MEMORY_COUNTERS_EX m0;
        DWORD h0 = 0;
        K32GetProcessMemoryInfo(me, (PPROCESS_MEMORY_COUNTERS)&m0, sizeof m0);
        GetProcessHandleCount(me, &h0);
        DWORD g0 = GetGuiResources(me, GR_GDIOBJECTS), u0 = GetGuiResources(me, GR_USEROBJECTS);
        for (int i = 0; i < reps; i++) {
            g_breakType = i & 1;
            g_breakEnd = now_ms() + 30000;
            overlays_create();
            mons = g_nov;
            for (int j = 0; j < g_nov; j++) SendMessageW(g_ov[j].hwnd, WM_PAINT, 0, 0);
            overlays_destroy();
            warn_show(i & 1, now_ms() + 15000);
            if (g_warn) SendMessageW(g_warn, WM_PAINT, 0, 0);
            warn_close();
            HWND dlg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, settings_proc, 0);
            if (dlg) DestroyWindow(dlg);
            if (apps) {
                collect_apps(apps);
                apps_load_icons(apps, 16 + 8 * (i & 1));
                HMENU am = build_app_menu(apps, L"steam.exe", 16 + 8 * (i & 1));
                if (am) DestroyMenu(am);
                apps_free_icons(apps);
            }
            g_fgHwnd = NULL;
            window_is_game(GetForegroundWindow());
            is_passthrough_window(GetForegroundWindow());
            if (!g_nid.hIcon) tray_load_icon(0); /* no real tray icon here: g_trayAdded stays 0 */
            tray_show_paused(!(i & 1));          /* normal <-> red bars every cycle */
            WCHAR st[STATUS_CCH];
            fmt_status(st, i & 1);
            HMENU mnu = CreatePopupMenu();
            AppendMenuW(mnu, MF_STRING, 1, st);
            DestroyMenu(mnu);
            if (i % 10 == 0) {
                audio_playing();
                InterlockedExchange64(&g_blockUntil, (LONGLONG)GetTickCount64() - 1); /* pass-through */
                hooks_start();
                hooks_stop();
            }
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
        PROCESS_MEMORY_COUNTERS_EX m1;
        DWORD h1 = 0;
        K32GetProcessMemoryInfo(me, (PPROCESS_MEMORY_COUNTERS)&m1, sizeof m1);
        GetProcessHandleCount(me, &h1);
        DWORD g1 = GetGuiResources(me, GR_GDIOBJECTS), u1 = GetGuiResources(me, GR_USEROBJECTS);
        if (pass == 1)
            n += wsprintfA(out + n,
                           "%d cycles (break screens on %d monitors, warning, settings window, app list, game check,"
                           " tray icon switch, sound check x%d, hook thread x%d):\r\n"
                           "   GDI objects %u -> %u | USER objects %u -> %u | kernel handles %u -> %u |"
                           " private memory %u KB -> %u KB\r\n",
                           reps, mons, (reps + 9) / 10, (reps + 9) / 10, (unsigned)g0, (unsigned)g1,
                           (unsigned)u0, (unsigned)u1, (unsigned)h0, (unsigned)h1,
                           (unsigned)(m0.PrivateUsage / 1024), (unsigned)(m1.PrivateUsage / 1024));
    }
    if (apps) {
        n += wsprintfA(out + n, "open apps offered by 'Choose app...': %d\r\n", apps->n);
        HeapFree(GetProcessHeap(), 0, apps);
    }
    lstrcpyW(g_logPath, logPath);

    /* 4. "Choose app..." add / remove logic on a hidden settings window */
    HWND dlg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, settings_proc, 0);
    if (dlg) {
        static const WCHAR *const steps[] = { L"MyGame.exe", L"retroarch.exe", L"MyGame.exe", L"overwatch.exe" };
        WCHAR txt[ARRAYSIZE(g_cfg.games)];
        SetDlgItemTextW(dlg, IDC_GAMES, L"Overwatch.exe; RetroArch");
        for (int i = 0; i < (int)ARRAYSIZE(steps); i++) {
            games_toggle(dlg, steps[i]);
            GetDlgItemTextW(dlg, IDC_GAMES, txt, ARRAYSIZE(txt));
            n += wsprintfA(out + n, "toggle %ls -> \"%ls\" (saved: \"%ls\")\r\n", steps[i], txt, g_cfg.games);
        }
        /* 5. the slider control drives the settings: keys, mouse (click + drag), wheel */
        HWND sl = GetDlgItem(dlg, IDC_MINI_EVERY);
        WCHAR val[32];
        RECT src;
        GetClientRect(sl, &src);
        SendMessageW(sl, TBM_SETPOS, TRUE, 9); /* 10 min */
        g_cfg.miniEveryMin = 10;
        SendMessageW(sl, WM_KEYDOWN, VK_RIGHT, 0);
        SendMessageW(sl, WM_KEYUP, VK_RIGHT, 0);
        GetDlgItemTextW(dlg, IDC_VAL_MINI_EVERY, val, ARRAYSIZE(val));
        n += wsprintfA(out + n, "slider: right arrow from 10 min -> saved %d min, label \"%ls\" (expect 15)\r\n", g_cfg.miniEveryMin, val);
        SendMessageW(sl, WM_KEYDOWN, VK_END, 0);
        SendMessageW(sl, WM_KEYUP, VK_END, 0);
        n += wsprintfA(out + n, "slider: End -> saved %d min (expect 120)\r\n", g_cfg.miniEveryMin);
        SendMessageW(sl, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, src.bottom / 2));
        SendMessageW(sl, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(src.right / 2, src.bottom / 2));
        int mid = g_dlgVal[0];
        SendMessageW(sl, WM_LBUTTONUP, 0, MAKELPARAM(src.right / 2, src.bottom / 2));
        n += wsprintfA(out + n, "slider: click at the left end, drag to the middle -> saved %d min (dragged to %d; expect ~25-35)\r\n",
                       g_cfg.miniEveryMin, mid);
        SendMessageW(sl, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA * 2), 0);
        KillTimer(dlg, DLG_TIMER_SAVE);
        dlg_save_values(dlg); /* the wheel saves after a short delay: do it now */
        n += wsprintfA(out + n, "slider: wheel up 2 notches -> saved %d min (2 steps up)\r\n", g_cfg.miniEveryMin);
        DestroyWindow(dlg);
    }
    HANDLE f = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, out, (DWORD)n, &wr, NULL); CloseHandle(f); }
}
#endif

#ifdef PAUSERICK_TEST_TASK
/* Test build only: the idle pause and the lock / unlock rules on a simulated clock
 * (5 s ticks, "watching" faked), so no real waiting and the user's input doesn't matter. */
static ULONGLONG idletest_sim(ULONGLONG from, ULONGLONG to, ULONGLONG lastInput, ULONGLONG watchUntil)
{
    ULONGLONG first = 0; /* first tick that is paused */
    for (ULONGLONG t = from; t <= to; t += IDLE_TICK_MS) {
        g_fakeWatching = t < watchUntil;
        if (idle_paused(NULL, t, lastInput) && !first) first = t;
    }
    return first;
}

static void idletest_reset(ULONGLONG t)
{
    g_pauseFlags = 0;
    g_activeSince = 0;
    g_fakeWatching = 0;
    idle_paused(NULL, t, t); /* someone typed: clears the detection state */
}

static void idletest(const WCHAR *outPath)
{
    static char out[4096];
    int n = 0, fails = 0;
    const ULONGLONG M = 60000, T0 = 1000 * M;
#define CHECK(label, got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); \
        if (g_ != w_) fails++; \
        n += wsprintfA(out + n, "%s  %s: %ld (expected %ld)\r\n", g_ == w_ ? "PASS" : "FAIL", label, (long)g_, (long)w_); } while (0)
    g_cfg.miniEnabled = 1; g_cfg.miniEveryMin = 30; g_cfg.longEnabled = 0;
    g_cfg.idlePause = 1; g_cfg.idlePauseMin = 3;

    for (int cont = 0; cont < 2; cont++) {
        g_cfg.idleContinue = cont;
        n += wsprintfA(out + n, "--- when back: %s\r\n", cont ? "continue" : "restart");
        /* user stops at T0 with 20 min left, comes back 20 min later */
        idletest_reset(T0 - M);
        g_miniRef = g_longRef = T0 - 10 * M;
        ULONGLONG first = idletest_sim(T0, T0 + 20 * M, T0, 0);
        CHECK("idle pause starts after (s)", (first - T0) / 1000, 180);
        CHECK("tray/status says paused", g_pauseFlags, PAUSE_IDLE);
        ULONGLONG back = T0 + 20 * M + 2000;
        CHECK("still paused after the first input", idle_paused(NULL, back + 3000, back), 0);
        CHECK("pause flags after input", g_pauseFlags, 0);
        CHECK("time left when back (s)", (g_miniRef + 30 * M - back) / 1000, cont ? 17 * 60 : 30 * 60);

        /* idle pause, then the PC gets locked, then unlocked */
        idletest_reset(T0 - M);
        g_miniRef = g_longRef = T0 - 10 * M;
        idletest_sim(T0, T0 + 5 * M, T0, 0);
        pause_begin(PAUSE_LOCK, T0 + 5 * M);
        pause_clear(PAUSE_LOCK | PAUSE_IDLE, T0 + 15 * M);
        CHECK("idle+lock, unlocked: flags", g_pauseFlags, 0);
        CHECK("idle+lock, unlocked: time left (s)", (g_miniRef + 30 * M - (T0 + 15 * M)) / 1000, cont ? 17 * 60 : 30 * 60);

        /* lock only: 2 min (shorter than the idle time), then 5 min */
        idletest_reset(T0 - M);
        g_miniRef = g_longRef = T0 - 10 * M;
        pause_begin(PAUSE_LOCK, T0);
        pause_clear(PAUSE_LOCK, T0 + 2 * M);
        CHECK("locked 2 min: time left (s)", (g_miniRef + 30 * M - (T0 + 2 * M)) / 1000, 20 * 60);
        g_miniRef = g_longRef = T0 - 10 * M;
        pause_begin(PAUSE_LOCK, T0);
        pause_clear(PAUSE_LOCK, T0 + 5 * M);
        CHECK("locked 5 min: time left (s)", (g_miniRef + 30 * M - (T0 + 5 * M)) / 1000, cont ? 20 * 60 : 30 * 60);
    }

    n += wsprintfA(out + n, "--- detection\r\n");
    g_cfg.idleContinue = 0;
    idletest_reset(T0 - M);
    CHECK("sound/fullscreen all along: paused", idletest_sim(T0, T0 + 20 * M, T0, (ULONGLONG)-1) != 0, 0);
    idletest_reset(T0 - M);
    CHECK("video ends at 10 min: pause after (s)", (idletest_sim(T0, T0 + 20 * M, T0, T0 + 10 * M) - T0) / 1000, 630);
    idletest_reset(T0 - M);
    g_activeSince = T0; /* unlocked at T0, last input an hour before */
    CHECK("unlocked, no input since: pause after (s)", (idletest_sim(T0, T0 + 5 * M, T0 - 60 * M, 0) - T0) / 1000, 180);
    idletest_reset(T0 - M);
    g_cfg.idlePause = 0;
    CHECK("idle pause off: paused", idletest_sim(T0, T0 + 20 * M, T0, 0) != 0, 0);
    g_miniRef = g_longRef = T0 - 10 * M;
    pause_begin(PAUSE_LOCK, T0);
    pause_clear(PAUSE_LOCK, T0 + 5 * M);
    CHECK("idle pause off, locked 5 min: time left (s)", (g_miniRef + 30 * M - (T0 + 5 * M)) / 1000, 20 * 60);
    g_cfg.idlePause = 1;
    idletest_reset(T0 - M);
    idletest_sim(T0, T0 + 5 * M, T0, 0);
    g_cfg.idlePause = 0; /* turned off while paused */
    CHECK("turned off while paused: still paused", idle_paused(NULL, T0 + 6 * M, T0), 0);
    CHECK("turned off while paused: flags", g_pauseFlags, 0);
#undef CHECK
    n += wsprintfA(out + n, "%s (%d failed)\r\n", fails ? "FAILED" : "ALL PASSED", fails);
    HANDLE f = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, out, (DWORD)n, &wr, NULL); CloseHandle(f); }
}
#endif

#ifdef PAUSERICK_TEST_TASK
static void write_out(const WCHAR *path, const char *text, int n)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, text, (DWORD)n, &wr, NULL); CloseHandle(f); }
}

/* Test build only: the whole update path against a local server ([Update] TestApi in the
 * ini): check, parse, download, signature, swap. The exe replaces itself, so the test
 * runs a copy. No dialog: it installs whatever newer version it finds. */
static void updatetest(const WCHAR *outPath)
{
    static char out[4096];
    static UpdInfo u;
    int n = 0;
    WCHAR old[MAX_PATH];
    update_fetch(&u);
    n += wsprintfA(out + n, "check: result=%d err=%lu web=%d version=%ls\r\nexe=%ls\r\nsig=%ls\r\nnotes=[%ls]\r\n", u.result,
                   u.err, u.web, u.version, u.exeUrl, u.sigUrl, u.notes);
    if (u.result == UPD_OK) {
        int r = update_apply(u.exeUrl, u.sigUrl, u.version);
        WIN32_FILE_ATTRIBUTE_DATA fa = { 0 };
        GetFileAttributesExW(g_exe, GetFileExInfoStandard, &fa);
        exe_sibling(old, g_exe, L".old.exe");
        n += wsprintfA(out + n, "apply: result=%d exe size now %lu, previous exe kept as .old.exe: %d\r\n", r,
                       fa.nFileSizeLow, GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES);
    }
    write_out(outPath, out, n);
}

/* Test build only: which breaks come when, over 4 simulated hours with the settings in the
 * ini, decided by the real next_break(); timers restart at the end of a break as in
 * break_teardown() (mini always, long after a long break). */
static void schedtest(const WCHAR *outPath)
{
    static char out[4096];
    int n = 0;
    ULONGLONG t0 = 1000000000ULL, t = t0, end = t0 + 4ULL * 3600 * 1000;
    g_miniRef = g_longRef = t0;
    n += wsprintfA(out + n, "mini every %d min for %d s, break every %d min for %d min\r\n", g_cfg.miniEveryMin,
                   g_cfg.miniDurSec, g_cfg.longEveryMin, g_cfg.longDurMin);
    while (t < end && n < (int)sizeof out - 100) {
        int type;
        ULONGLONG at;
        if (!next_break(&type, &at)) break;
        if (at > t) t = at;
        ULONGLONG dur = type == BT_LONG ? (ULONGLONG)g_cfg.longDurMin * 60000 : (ULONGLONG)g_cfg.miniDurSec * 1000;
        unsigned s = (unsigned)((t - t0) / 1000);
        n += wsprintfA(out + n, "%u:%02u:%02u  %s\r\n", s / 3600, (s / 60) % 60, s % 60, type == BT_LONG ? "BREAK" : "mini break");
        t += dur;
        g_miniRef = t;
        if (type == BT_LONG) g_longRef = t;
    }
    write_out(outPath, out, n);
}

/* Test build only: the update dialog with made-up data, to look at it. */
static void prompttest(void)
{
    lstrcpyW(g_upd.version, L"1.5.0");
    lstrcpyW(g_upd.exeUrl, L"https://127.0.0.1/none");
    lstrcpyW(g_upd.sigUrl, L"https://127.0.0.1/none");
    lstrcpyW(g_upd.notes, L"- Example change one\n- Example change two");
    update_prompt();
}

/* Test build only: --render <dir> <width> <height> <dpi>. The break screen and the warning,
 * drawn by their own paint code into bitmaps (no window ever shows), as frames for the
 * README animation: break_30.bmp ... break_01.bmp and warn_5.bmp ... warn_1.bmp. */
static void render_save(const WCHAR *path, const void *bits, int w, int h)
{
    BITMAPFILEHEADER fh = { 0x4D42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
    BITMAPINFOHEADER ih = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
    DWORD size = (DWORD)w * (DWORD)h * 4, wr;
    fh.bfSize = fh.bfOffBits + size;
    GdiFlush();
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, &fh, sizeof fh, &wr, NULL);
    WriteFile(f, &ih, sizeof ih, &wr, NULL);
    WriteFile(f, bits, size, &wr, NULL);
    CloseHandle(f);
}

static HBITMAP render_dib(HDC dc, int w, int h, void **bits)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down, like render_save writes it */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

static void render_frames(const WCHAR *dir, int w, int h, int dpi)
{
    WCHAR path[MAX_PATH];
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return;

    Overlay o;
    ZeroMemory(&o, sizeof o);
    overlay_layout(&o, w, h); /* no window: text metrics come from the screen DC */
    HBITMAP bmp = render_dib(dc, w, h, &bits);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT rc = { 0, 0, w, h };
        g_breakType = BT_MINI;
        for (int s = 30; s >= 1; s--) {
            g_breakEnd = now_ms() + (ULONGLONG)s * 1000 - 500;
            overlay_paint(&o, dc, &rc);
            wsprintfW(path, L"%s\\break_%02d.bmp", dir, s);
            render_save(path, bits, w, h);
        }
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    if (o.fTitle) DeleteObject(o.fTitle);
    if (o.fTime) DeleteObject(o.fTime);

    /* the warning at the size warn_show() gives it on a monitor with this DPI */
    int ww = MulDiv(250, dpi, 96), wh = MulDiv(54, dpi, 96);
    g_warnDpi = (UINT)dpi;
    g_warnFont = make_font(MulDiv(17, dpi, 96), FW_SEMIBOLD, L"Segoe UI", CLEARTYPE_QUALITY);
    bits = NULL;
    bmp = render_dib(dc, ww, wh, &bits);
    if (bmp && bits && g_warnFont) {
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT rc = { 0, 0, ww, wh };
        g_warnType = BT_MINI;
        for (int s = 5; s >= 1; s--) {
            g_warnAt = now_ms() + (ULONGLONG)s * 1000 - 500;
            warn_draw(dc, rc);
            wsprintfW(path, L"%s\\warn_%d.bmp", dir, s);
            render_save(path, bits, ww, wh);
        }
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    if (g_warnFont) { DeleteObject(g_warnFont); g_warnFont = NULL; }
    DeleteDC(dc);
}

/* Test build only: Block Task Manager on / off through the real helper flow. */
static void stricttest(const WCHAR *outPath, int on)
{
    static char out[1024];
    int n, r = on ? strict_install(1) : task_delete();
    WCHAR te[MAX_PATH];
    strict_task_exe(te);
    n = wsprintfA(out, "%s: result=%d StrictTaskExe=[%ls] exists=%d at install path=%d\r\n", on ? "on" : "off", r, te,
                  te[0] && GetFileAttributesW(te) != INVALID_FILE_ATTRIBUTES, strict_at_install_path());
    write_out(outPath, out, n);
}
#endif

#ifdef PAUSERICK_TEST_TASK
/* Test build only: show the app menu with made-up entries (never the user's real
 * windows) at x,y for 4 seconds, so it can be captured on screen. */
static void CALLBACK menutest_close(HWND h, UINT m, UINT_PTR id, DWORD t) { (void)h; (void)m; (void)t; KillTimer(NULL, id); EndMenu(); }

static void menutest(int x, int y)
{
    static const WCHAR *const exes[][2] = {
        { L"C:\\Program Files (x86)\\Steam\\steam.exe", L"Steam" },
        { L"%EXE%", L"Pauserick" },
        { L"C:\\Windows\\explorer.exe", L"Example folder" },
        { L"C:\\Windows\\System32\\cmd.exe", L"Example console" },
        { L"C:\\Windows\\System32\\notepad.exe", L"Example text" },
        { L"C:\\nonexistent\\noicon.exe", L"An app without an icon" },
    };
    AppList *l = (AppList *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *l);
    if (!l) return;
    for (int i = 0; i < (int)ARRAYSIZE(exes); i++) {
        const WCHAR *p = lstrcmpW(exes[i][0], L"%EXE%") ? exes[i][0] : g_exe;
        const WCHAR *base = wcsrchr(p, L'\\');
        lstrcpynW(l->e[l->n].path, p, MAX_PATH);
        lstrcpynW(l->e[l->n].name, base ? base + 1 : p, 64);
        lstrcpynW(l->e[l->n].title, exes[i][1], 48);
        l->n++;
    }
    UINT dpi = 96, dy;
    GetDpiForMonitor(MonitorFromPoint((POINT){ x, y }, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpi, &dy);
    int cx = GetSystemMetricsForDpi(SM_CXSMICON, (UINT)dpi);
    apps_load_icons(l, cx);
    HMENU m = build_app_menu(l, L"steam.exe; cmd", cx);
    SetForegroundWindow(g_main);
    SetTimer(NULL, 0, 4000, menutest_close);
    TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, x, y, 0, g_main, NULL);
    DestroyMenu(m);
    apps_free_icons(l);
    HeapFree(GetProcessHeap(), 0, l);
}
#endif

/* Last safety net: should anything ever crash, Windows removes the input hooks
 * together with the process, and this puts Task Manager back right away. */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *e)
{
    InterlockedExchange64(&g_blockUntil, 0); /* first make the PC usable, then write the log */
    taskmgr_unlock();
    if (e && e->ExceptionRecord) {
        ULONG_PTR at = (ULONG_PTR)e->ExceptionRecord->ExceptionAddress, base = (ULONG_PTR)g_inst;
        if (at >= base && at - base < 0x1000000)
            log_error(L"CRASH: exception 0x%08lX at Pauserick.exe+0x%lX; keyboard, mouse and Task Manager released",
                      (unsigned long)e->ExceptionRecord->ExceptionCode, (unsigned long)(at - base));
        else
            log_error(L"CRASH: exception 0x%08lX at %p (outside Pauserick.exe); keyboard, mouse and Task Manager released",
                      (unsigned long)e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void register_class(const WCHAR *name, WNDPROC proc, HCURSOR cur)
{
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = proc;
    wc.hInstance = g_inst;
    wc.hCursor = cur;
    wc.lpszClassName = name;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdLine, int show)
{
    (void)prev; (void)cmdLine; (void)show;
    g_inst = hi;
    SetUnhandledExceptionFilter(crash_handler);
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    g_elevated = is_elevated();

    /* command line: [--config <file.ini>] [--mini-break | --break] [--from-task]
     * Helper modes, started elevated by Pauserick itself:
     *   --strict-install <logon 0|1> <user SID> <custom ini 0|1>   (Block Task Manager on)
     *   --apply-update <exe URL> <signature URL> <version>          (install a signed update) */
    int argc = 0, breakArg = -1, strictLogon = -1, strictCustom = 0;
    WCHAR strictSid[200] = L"", updExe[512] = L"", updSig[512] = L"", updVer[24] = L"";
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--config") && i + 1 < argc) {
            GetFullPathNameW(argv[++i], MAX_PATH, g_ini, NULL);
            g_customIni = 1;
        } else if (!lstrcmpiW(argv[i], L"--mini-break")) breakArg = BT_MINI;
        else if (!lstrcmpiW(argv[i], L"--break"))        breakArg = BT_LONG;
        else if (!lstrcmpiW(argv[i], L"--from-task"))    g_fromTask = 1;
        else if (!lstrcmpiW(argv[i], L"--strict-install") && i + 3 < argc) {
            strictLogon = argv[i + 1][0] == L'1';
            lstrcpynW(strictSid, argv[i + 2], ARRAYSIZE(strictSid));
            strictCustom = argv[i + 3][0] == L'1';
            i += 3;
        } else if (!lstrcmpiW(argv[i], L"--apply-update") && i + 3 < argc) {
            lstrcpynW(updExe, argv[i + 1], ARRAYSIZE(updExe));
            lstrcpynW(updSig, argv[i + 2], ARRAYSIZE(updSig));
            lstrcpynW(updVer, argv[i + 3], ARRAYSIZE(updVer));
            i += 3;
        }
    }
#ifdef PAUSERICK_TEST_TASK
    const WCHAR *selftestOut = NULL;
    int selftestCycles = 200, selftestAudio = 0, menutestX = -1, menutestY = 0, pauseMediaTest = 0;
    for (int i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--quiet-break")) g_show = 0; /* hidden break screens, input not blocked */
        if (!lstrcmpiW(argv[i], L"--pause-media")) pauseMediaTest = 1; /* only what a break start does to media */
        if (!lstrcmpiW(argv[i], L"--timing-log")) g_timingLog = 1;
        if (!lstrcmpiW(argv[i], L"--offscreen")) g_offscreen = 1;
    }
    for (int i = 1; argv && i + 2 < argc; i++)
        if (!lstrcmpiW(argv[i], L"--menutest")) {
            menutestX = (int)wcstol(argv[i + 1], NULL, 10);
            menutestY = (int)wcstol(argv[i + 2], NULL, 10);
        }
    const WCHAR *idletestOut = NULL, *updtestOut = NULL, *stricttestOut = NULL, *schedtestOut = NULL;
    int prompttestOn = 0, stricttestOn = 0;
    for (int i = 1; argv && i + 1 < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--idletest")) idletestOut = argv[i + 1];
        if (!lstrcmpiW(argv[i], L"--update-test")) updtestOut = argv[i + 1];
        if (!lstrcmpiW(argv[i], L"--schedtest")) schedtestOut = argv[i + 1];
        if (!lstrcmpiW(argv[i], L"--strict-on-test")) { stricttestOut = argv[i + 1]; stricttestOn = 1; }
        if (!lstrcmpiW(argv[i], L"--strict-off-test")) stricttestOut = argv[i + 1];
    }
    for (int i = 1; argv && i < argc; i++)
        if (!lstrcmpiW(argv[i], L"--prompt-test")) prompttestOn = 1;
    const WCHAR *renderDir = NULL;
    int renderW = 1920, renderH = 1080, renderDpi = 96;
    for (int i = 1; argv && i + 4 < argc; i++)
        if (!lstrcmpiW(argv[i], L"--render")) {
            renderDir = argv[i + 1];
            renderW = clampi((int)wcstol(argv[i + 2], NULL, 10), 320, 7680);
            renderH = clampi((int)wcstol(argv[i + 3], NULL, 10), 240, 4320);
            renderDpi = clampi((int)wcstol(argv[i + 4], NULL, 10), 96, 384);
        }
    for (int i = 1; argv && i + 1 < argc; i++)
        if (!lstrcmpiW(argv[i], L"--selftest")) {
            selftestOut = argv[i + 1];
            if (i + 2 < argc) selftestCycles = clampi((int)wcstoul(argv[i + 2], NULL, 10), 10, 100000);
            if (i + 3 < argc) selftestAudio = !lstrcmpiW(argv[i + 3], L"audio");
        }
    /* test build only: --config <ini> --probe <out.txt> [hwnd]
     * writes what gaming mode / idle detection see for that window (default: foreground) */
    for (int i = 1; argv && i + 1 < argc; i++) {
        if (lstrcmpiW(argv[i], L"--probe")) continue;
        HWND w = i + 2 < argc ? (HWND)(ULONG_PTR)wcstoul(argv[i + 2], NULL, 10) : GetForegroundWindow();
        settings_load();
        int game = window_is_game(w), kind = g_fgKind, full = covers_monitor(w);
        Sleep(1500); /* the test samples memory before the first audio check */
        int audio = audio_playing();
        char out[160];
        int n = wsprintfA(out, "game=%d kind=%d fullscreen=%d audio=%d watching=%d\r\n",
                          game, kind, full, audio, full || audio);
        HANDLE f = CreateFileW(argv[i + 1], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD wr;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, out, (DWORD)n, &wr, NULL); CloseHandle(f); }
        Sleep(2500); /* ...and after it */
        LocalFree(argv);
        return 0;
    }
#endif
#ifndef PAUSERICK_TEST_TASK
    if (argv) LocalFree(argv);
#endif

    /* settings.ini (and pauserick.log next to it) */
    if (!g_customIni) {
        WCHAR dir[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH - 32);
        if (n && n < MAX_PATH - 32) {
            lstrcatW(dir, L"\\Pauserick");
            CreateDirectoryW(dir, NULL);
        } else { /* no %APPDATA%: keep settings next to the exe */
            GetModuleFileNameW(NULL, dir, MAX_PATH - 32);
            for (WCHAR *p = dir + lstrlenW(dir); p > dir; p--)
                if (p[-1] == L'\\') { p[-1] = 0; break; }
        }
        wsprintfW(g_ini, L"%s\\settings.ini", dir);
    }
    log_init();

    /* elevated helpers: do the one job and exit, no window, no tray icon */
    if (strictLogon >= 0) {
        g_customIni = strictCustom; /* --config was only passed so we use the same settings file */
        if (TASK_ELEVATE && !g_elevated) return 1;
        return strict_install_elevated(strictLogon, strictSid) ? 0 : 1;
    }
    if (updExe[0]) return update_apply(updExe, updSig, updVer);
#ifdef PAUSERICK_TEST_TASK
    /* test hooks that must run before the Block Task Manager relaunch below */
    if (updtestOut) { settings_load(); updatetest(updtestOut); return 0; }
    if (schedtestOut) { settings_load(); schedtest(schedtestOut); return 0; }
    if (stricttestOut) { settings_load(); stricttest(stricttestOut, stricttestOn); return 0; }
    if (prompttestOn) { settings_load(); prompttest(); return 0; }
    if (renderDir) { settings_load(); render_frames(renderDir, renderW, renderH, renderDpi); return 0; }
#endif

    g_mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    DWORD mutexErr = GetLastError();
    /* ACCESS_DENIED: the running instance is elevated (strict mode) and we are not */
    if (mutexErr == ERROR_ALREADY_EXISTS || (!g_mutex && mutexErr == ERROR_ACCESS_DENIED)) {
        HWND other = FindWindowW(CLS_MAIN, NULL);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            if (breakArg >= 0) PostMessageW(other, WM_APP_BREAK_NOW, (WPARAM)breakArg, 0);
            else               PostMessageW(other, WM_APP_SHOW_SETTINGS, 0, 0);
            log_info(L"Launched again while already running (pid %lu): asked it to %s", pid,
                     breakArg >= 0 ? L"start a break" : L"open Settings");
        }
        return 0;
    }

    g_firstRun = GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES;
    settings_load();

    /* Strict mode is set up but we were started normally (double-click, any copy): let the
     * elevated task run the installed copy instead. --from-task prevents any loop. */
    WCHAR taskExe[MAX_PATH];
    strict_task_exe(taskExe);
    if (!g_elevated && !g_fromTask && taskExe[0] && GetFileAttributesW(taskExe) != INVALID_FILE_ATTRIBUTES) {
        /* a newer exe than the installed one (downloaded by hand): install it first (one UAC prompt) */
        int inst[3], mine[3];
        if (lstrcmpiW(g_exe, taskExe) && file_version(taskExe, inst) && ver_parse(APP_VERSION, mine) &&
            ver_cmp3(mine, inst) > 0) {
            log_info(L"Started %s, newer than the installed %d.%d.%d: installing it", g_exe, inst[0], inst[1], inst[2]);
            if (!strict_install(GetPrivateProfileIntW(L"State", L"StrictAutostart", 1, g_ini) != 0))
                log_warn(L"Not installed: starting the installed version instead");
        }
        if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
        if (task_run()) {
            log_info(L"Started without administrator rights while Block Task Manager is on: relaunched through the task");
            return 0;
        }
        log_warn(L"Block Task Manager is on but the task could not be started: running without administrator rights");
        g_mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME); /* task failed: run normally */
    }
    /* Set up by an older version, the task may start Pauserick from a folder anyone can
     * change. We have the rights now: move it into the protected folder and hand over. */
    if (g_elevated && taskExe[0] && !strict_at_install_path()) {
        WCHAR sid[200], dir[MAX_PATH], inst[MAX_PATH];
        int logon = GetPrivateProfileIntW(L"State", L"StrictAutostart", 1, g_ini) != 0;
        log_info(L"Block Task Manager: moving Pauserick from %s into a folder only administrators can change", taskExe);
        if (current_user_sid(sid, ARRAYSIZE(sid)) && strict_install_elevated(logon, sid) &&
            strict_install_path(dir, inst) && lstrcmpiW(g_exe, inst)) {
            if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
            if (task_run()) {
                log_info(L"Handing over to %s", inst);
                return 0;
            }
            g_mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
        }
    }

    log_info(L"Pauserick " APP_VERSION L" started (pid %lu, administrator rights: %s%s, %d monitor(s))",
             GetCurrentProcessId(), g_elevated ? L"yes" : L"no", g_fromTask ? L", via the startup task" : L"",
             GetSystemMetrics(SM_CMONITORS));
    log_info(L"Settings file: %s%s", g_ini, g_firstRun ? L" (first run: defaults)" : L"");
    log_settings(L"Settings", &g_cfg);
    log_info(L"Block Task Manager %s | Start with Windows %s", onoff(strict_registered()), onoff(startup_enabled()));
    {
        WCHAR from[24];
        GetPrivateProfileStringW(L"Update", L"From", L"", from, ARRAYSIZE(from), g_ini);
        if (from[0]) {
            log_info(L"Updated from %s to %s", from, APP_VERSION);
            WritePrivateProfileStringW(L"Update", L"From", NULL, g_ini);
        }
    }
    update_cleanup();

    /* Undo a Task Manager lock left behind by a crash or power loss mid-break. */
    if (GetPrivateProfileIntW(L"State", L"TaskManagerLocked", 0, g_ini)) {
        log_warn(L"Task Manager was left disabled by a previous session that ended abruptly: restoring it");
        g_taskMgrLocked = 1;
        taskmgr_unlock();
    }

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    register_class(CLS_MAIN, main_proc, NULL);
    register_class(CLS_OVERLAY, overlay_proc, NULL);
    register_class(CLS_WARN, warn_proc, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
    slider_register(); /* the settings window's sliders */
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, CLS_MAIN, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!g_main) {
        log_error(L"Could not create the main window (error %lu): exiting", GetLastError());
        return 1;
    }
    /* When elevated, let Explorer (tray, restarts) and normal-privilege launches reach us. */
    static const UINT allowed[] = { WM_APP_TRAY, WM_APP_SHOW_SETTINGS, WM_APP_BREAK_NOW };
    for (int i = 0; i < (int)ARRAYSIZE(allowed); i++)
        ChangeWindowMessageFilterEx(g_main, allowed[i], MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_main, g_msgTaskbarCreated, MSGFLT_ALLOW, NULL);
#ifdef PAUSERICK_TEST_TASK
    if (selftestOut) { selftest(selftestOut, selftestCycles, selftestAudio); return 0; }
    if (idletestOut) { idletest(idletestOut); return 0; }
    if (menutestX >= 0) { menutest(menutestX, menutestY); return 0; }
    if (pauseMediaTest) { media_pause_thread(NULL); return 0; }
#endif
    WTSRegisterSessionNotification(g_main, NOTIFY_FOR_THIS_SESSION);
#ifdef PAUSERICK_TEST_TASK
    if (!g_timingLog) /* timing test: no tray icon, nothing on screen */
#endif
    tray_add();

    g_miniRef = g_longRef = now_ms();
    if (g_firstRun) {
        settings_save();
        autostart_set(1);
        show_settings();
    } else {
        autostart_fix_path();
        /* we were started by a previous instance that just enabled strict mode */
        if (GetPrivateProfileIntW(L"State", L"OpenSettings", 0, g_ini)) {
            WritePrivateProfileStringW(L"State", L"OpenSettings", NULL, g_ini);
            show_settings();
        }
    }

    set_eco(TRUE);
    SetCoalescableTimer(g_main, TIMER_UPDATE, UPDATE_FIRST_MS, NULL, 10000); /* first update look */
    tick();
    if (breakArg >= 0) start_break(breakArg, L"from the command line");
    trim_memory();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_settings && IsDialogMessageW(g_settings, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
