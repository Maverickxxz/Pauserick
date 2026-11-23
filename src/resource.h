#pragma once

#define IDI_APP             1
#define IDI_PAUSED          2   /* tray icon while a game / listed app holds the breaks, or the PC is idle */
#define IDD_SETTINGS        100

/* sliders (trackbars) and the value label next to each one */
#define IDC_MINI_EVERY      1002
#define IDC_VAL_MINI_EVERY  1003
#define IDC_MINI_DUR        1004
#define IDC_VAL_MINI_DUR    1005
#define IDC_LONG_EVERY      1012
#define IDC_VAL_LONG_EVERY  1013
#define IDC_LONG_DUR        1014
#define IDC_VAL_LONG_DUR    1015
#define IDC_WARN            1024
#define IDC_VAL_WARN        1025
#define IDC_IDLE            1026 /* pause if idle for... */
#define IDC_VAL_IDLE        1027

#define IDC_MINI_ENABLE     1001
#define IDC_LONG_ENABLE     1011
#define IDC_AUTOSTART       1021
#define IDC_STRICT          1022 /* "Block Task Manager during breaks" */
#define IDC_SOUND           1023
#define IDC_LANG            1028
#define IDC_GAMING          1029 /* gaming mode */
#define IDC_GAMES           1030 /* extra games: exe names / folders */
#define IDC_PICK_GAMES      1032 /* "Choose app..." (list of open apps) */
#define IDC_IDLE_ENABLE     1033 /* "Pause if idle for" (its slider is IDC_IDLE) */
#define IDC_RESUME          1034 /* when back from an idle pause: restart / continue */
#define IDC_AUTOUPDATE      1035 /* check for updates every day */
#define IDC_UPDATE          1036 /* header link: check now / install the new version */

#define IDC_STATUS          1031

/* cards: hidden group boxes, only their position is used (the card, its icon, title and
 * description are painted by the dialog) */
#define IDC_GRP_MINI        1101
#define IDC_GRP_LONG        1111
#define IDC_GRP_GENERAL     1121
#define IDC_GRP_IDLE        1127
#define IDC_GRP_GAMING      1131

/* static labels (translated at runtime) */
#define IDC_LBL_MINI_EVERY  1102
#define IDC_LBL_MINI_DUR    1104
#define IDC_LBL_LONG_EVERY  1112
#define IDC_LBL_LONG_DUR    1114
#define IDC_LBL_WARN        1122
#define IDC_LBL_IDLE_AFTER  1124
#define IDC_LBL_RESUME      1125
#define IDC_LBL_LANG        1126
#define IDC_LBL_GAMES       1132
#define IDC_LBL_GAMES_AUTO  1133 /* which games are recognized by themselves */
