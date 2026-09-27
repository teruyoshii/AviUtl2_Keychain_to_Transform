//----------------------------------------------------------------------------------
//	Blender風のショートカットキーチェーンでオブジェクトを変形するプラグイン for AviUtl ExEdit2
//
//	概要:
//		編集メニューに登録した「移動」「中心移動」「回転」「拡大」コマンド(本体のショートカットキー設定で
//		G/R/S等を割り当てる)を起点に、軸指定(X/Y/Z、Shift+で平面)・マウス移動・ホイール・数値入力で
//		選択中オブジェクトの標準描画(標準描画が無ければグループ制御等。Alt指定中や時間変化する項目はエフェクト)の値を変更する。
//		操作中のみメインスレッド限定のキーボード/マウスフックを張り、入力を横取りする。
//
//	v0.1 By teruyoshi
//----------------------------------------------------------------------------------
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sstream>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

#define HUD_CLASS_NAME L"KeychainToTransformHud"				// 操作中の状態表示ウィンドウ(タイマーの受け口も兼ねる)
#define CONFIG_DLG_CLASS_NAME L"KeychainToTransformConfigDialog"	// 「設定」メニューから開く設定ダイアログ
#define WM_APP_FINISH (WM_APP + 1)			// wparam: 1=確定 0=キャンセル
#define TIMER_ID_POLL 1

//---------------------------------------------------------------------
//	設定 (「設定」→「Keychain to Transform設定」で変更し、%AppData%\Plugin\KeychainToTransformSettings.iniに保存する)
//	感度はプレビューの表示倍率を取得できないため、画面上の1pxあたりの変化量で指定する
//---------------------------------------------------------------------
struct Settings {
	double move_per_px = 1.0;		// 移動: 1pxあたりの座標変化量
	double rotate_per_px = 0.5;		// 回転: 1pxあたりの角度(度)
	double scale_per_px = 0.005;	// 拡大: 1pxあたりの倍率変化
	double move_per_wheel = 10.0;	// 移動(軸指定無し): ホイール1ノッチあたりのZ変化量
	double rotate_per_wheel = 5.0;	// カメラ制御の回転(軸指定無し): ホイール1ノッチあたりのZ軸回転の角度(度)
	double fine_ratio = 0.1;		// Shift押下中の倍率 (微調整)
	double coarse_ratio = 10.0;		// Ctrl押下中の倍率 (粗調整)
	double both_ratio = 0.01;		// Shift+Ctrl押下中の倍率
	int poll_ms = 33;				// マウス位置の読み取り・値の反映間隔(ミリ秒)。長くするとUndoに積まれる件数が減る
	bool invert_camera_pitch = false;	// カメラ制御の上下の向き(X軸回転)で、マウスの縦方向を反転する
	bool invert_wheel = false;		// ホイールの向きを反転する
	// 拡大率の代わりに優先して変更する大きさの項目名 (カンマ区切り、左を優先)。[0]=X [1]=Y [2]=Z [3]=全体
	std::wstring size_items[4] = { L"幅,X幅", L"高さ,Y幅", L"奥行き,Z幅", L"サイズ" };
};
Settings g_settings;

// マウス・ホイールの変化量に掛ける、修飾キーによる倍率 (Shift/Ctrl/Shift+Ctrlそれぞれ設定値)
double modifier_ratio() {
	bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	if (shift && ctrl) return g_settings.both_ratio;
	if (shift) return g_settings.fine_ratio;
	if (ctrl) return g_settings.coarse_ratio;
	return 1.0;
}
const wchar_t* SIZE_ITEM_KEYS[4] = { L"size_items_x", L"size_items_y", L"size_items_z", L"size_items_all" };

// カンマ区切り(「,」「、」)の項目名を分割する (前後の空白は除く)
std::vector<std::wstring> split_item_names(const std::wstring& text) {
	std::vector<std::wstring> names;
	std::wstring current;
	auto flush = [&]() {
		size_t first = current.find_first_not_of(L" \t　");
		size_t last = current.find_last_not_of(L" \t　");
		if (first != std::wstring::npos) names.push_back(current.substr(first, last - first + 1));
		current.clear();
	};
	for (wchar_t c : text) {
		if (c == L',' || c == L'、') flush();
		else current += c;
	}
	flush();
	return names;
}

//---------------------------------------------------------------------
//	グローバルハンドル
//---------------------------------------------------------------------
EDIT_HANDLE* edit_handle = nullptr;
LOG_HANDLE* logger = nullptr;
CONFIG_HANDLE* config = nullptr;
HWND g_hud = nullptr;
HFONT g_hud_font = nullptr;
HHOOK g_keyboard_hook = nullptr;
HHOOK g_mouse_hook = nullptr;

//---------------------------------------------------------------------
//	トラックバー項目の設定値 (エイリアスファイルと同じ書式)
//	  移動無し : "100.00"
//	  移動あり : "始点,中間点…,終点,移動方法名,フラグ|パラメータ" (値はキーの数=区間数+1個)
//	  式あり   : "0.00,移動無し,8|式"
//---------------------------------------------------------------------
struct TrackValue {
	bool valid = false;				// 先頭に1つ以上の数値があり解釈できたか
	std::string raw;				// 元の文字列
	std::vector<std::string> keys;	// キーごとの値の文字列 (始点・中間点・終点の順。移動無しは1つ)
	std::string suffix;				// 数値列より後ろの部分 (",直線移動,0|…"。単一の数値のみの場合は空)
	bool moving = false;			// 移動方法が設定されているか (get_object_track_infoのmodeがnullptr以外)

	// 単一の数値のみ (時間変化も式も無い)
	bool plain() const { return valid && keys.size() == 1 && suffix.empty(); }
	double key(int k) const { return strtod(keys[k].c_str(), nullptr); }

	// キーkの値をvalueに差し替えた文字列
	std::string text_with(int k, const std::string& value) const {
		std::string s;
		for (int i = 0; i < (int)keys.size(); i++) {
			if (i > 0) s += ",";
			s += (i == k) ? value : keys[i];
		}
		return s + suffix;
	}
};

bool is_number_token(const std::string& token) {
	if (token.empty()) return false;
	char* end = nullptr;
	strtod(token.c_str(), &end);
	return end != token.c_str() && *end == '\0';
}

bool parse_track_value(LPCSTR s, TrackValue& tv) {
	tv = TrackValue{};
	if (!s) return false;
	tv.raw = s;
	size_t bar = tv.raw.find('|');
	std::string head = tv.raw.substr(0, bar);
	std::string tail = (bar == std::string::npos) ? "" : tv.raw.substr(bar);
	size_t pos = 0;
	while (pos <= head.size()) {
		size_t comma = head.find(',', pos);
		std::string token = head.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
		if (!is_number_token(token)) {
			// 数値でない最初の要素(移動方法名)の直前の「,」から後ろを丸ごと保持する
			if (pos > 0) tv.suffix = head.substr(pos - 1);
			break;
		}
		tv.keys.push_back(token);
		if (comma == std::string::npos) break;
		pos = comma + 1;
	}
	tv.suffix += tail;
	tv.valid = !tv.keys.empty();
	return tv.valid;
}

// 数値を、元の値と同じ小数点以下の桁数で文字列化する (項目ごとの桁数は保存値の書式から判断する。
// 例: 標準描画のXは"0.00"、拡大率は"100.000"、基本出力のXは"0")。
// 元の値と同じ表示になる場合は元の文字列をそのまま返す
std::string format_value(double value, const std::string& original) {
	size_t dot = original.find('.');
	int decimals = (dot == std::string::npos) ? 0 : (int)(original.size() - dot - 1);
	char buf[64];
	snprintf(buf, sizeof(buf), "%.*f", decimals, value);
	if (strcmp(buf, original.c_str()) == 0) return original;
	if (strtod(buf, nullptr) == 0) snprintf(buf, sizeof(buf), "%.*f", decimals, 0.0); // "-0.00"を避ける
	return buf;
}

//---------------------------------------------------------------------
//	対象項目 (オブジェクトの基本のエフェクト)
//	標準描画を持たないオブジェクトも、位置・回転・拡大率に相当する項目を持つ下記のエフェクトを変更対象にする。
//	オブジェクトごとに先頭から順に探し、最初に見つかったものを使う。
//	各項目に対応する項目名をエフェクトごとに持ち、nullptrの項目は変更しない。
//	以降、コメント中の「標準描画」はこの基本のエフェクトを指す
//---------------------------------------------------------------------
// X,Y,Zの3項目は連続して並べる (base_item()からの軸番号オフセットで参照するため)
enum ItemIndex { ITEM_X, ITEM_Y, ITEM_Z, ITEM_CX, ITEM_CY, ITEM_CZ, ITEM_RX, ITEM_RY, ITEM_RZ, ITEM_ZOOM, ITEM_NUM };
const wchar_t* ITEM_NAMES[ITEM_NUM] = { L"X", L"Y", L"Z", L"中心X", L"中心Y", L"中心Z", L"X軸回転", L"Y軸回転", L"Z軸回転", L"拡大率" };

struct BaseEffectDef {
	const wchar_t* name;
	const wchar_t* items[ITEM_NUM];	// 各項目に対応する項目名 (nullptr=持たない)
	const wchar_t* center_label;	// HUDでの中心の表示名
	bool center_by_effect;			// 中心を持たないため、中心移動は常に中心座標エフェクトで行う
	bool camera;					// カメラ制御 (拡大は視野角を倍率で割る、X/Y軸回転は目標点を回転させる)
};
const wchar_t* STANDARD_DRAW = L"標準描画";
const BaseEffectDef BASE_EFFECTS[] = {
	{ STANDARD_DRAW, { L"X", L"Y", L"Z", L"中心X", L"中心Y", L"中心Z", L"X軸回転", L"Y軸回転", L"Z軸回転", L"拡大率" }, L"中心", false, false },
	{ L"映像再生", { L"X", L"Y", L"Z", L"中心X", L"中心Y", L"中心Z", L"X軸回転", L"Y軸回転", L"Z軸回転", L"拡大率" }, L"中心", false, false },
	{ L"グループ制御", { L"X", L"Y", L"Z", nullptr, nullptr, nullptr, L"X軸回転", L"Y軸回転", L"Z軸回転", L"拡大率" }, L"中心", true, false },
	// カメラ制御: 中心→目標、Z軸回転→傾き、拡大率→視野角 と読み替える。X/Y軸回転は目標点を回転させて行う
	{ L"カメラ制御", { L"X", L"Y", L"Z", L"目標X", L"目標Y", L"目標Z", nullptr, nullptr, L"傾き", L"視野角" }, L"目標", false, true },
	{ L"基本出力", { L"X", L"Y", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, L"拡大率" }, L"中心", false, false },
};

// 上記のいずれも持たないオブジェクト(点光源・スポット光源等)は、エフェクト一覧の一番上のエフェクトを変更対象にし、
// 項目ごとに下記の候補を先頭から探して、最初に見つかった項目名を使う (中心は「中心X」等が無ければ「目標X」等)
const BaseEffectDef GENERIC_BASE = { nullptr, {}, L"中心", false, false };
const wchar_t* GENERIC_ITEM_CANDIDATES[ITEM_NUM][2] = {
	{ L"X", nullptr }, { L"Y", nullptr }, { L"Z", nullptr },
	{ L"中心X", L"目標X" }, { L"中心Y", L"目標Y" }, { L"中心Z", L"目標Z" },
	{ L"X軸回転", nullptr }, { L"Y軸回転", nullptr }, { L"Z軸回転", nullptr },
	{ L"拡大率", nullptr },
};

//---------------------------------------------------------------------
//	対象項目 (エフェクト)
//	Alt指定中、または変更する標準描画の項目が時間変化する場合は、標準描画の代わりにこれらのエフェクトを変更する。
//	拡大の軸指定は標準描画に軸別の拡大率が無いため、常に拡大率エフェクトを変更する。
//	中心移動の「中心座標」は標準エフェクトではなく、sikemoti氏の「中心座標」スクリプト(MIT License)を改名して同梱したもの。
//	(利用者が元の「中心座標」を別途導入している場合と名前が衝突しないよう改名している。導入されていなければ追加に失敗し変更しない)
//	X,Y,Zの3項目は先頭に並べる (軸番号で参照するため)
//---------------------------------------------------------------------
enum EffectKind { FX_MOVE, FX_CENTER, FX_ROTATE, FX_SCALE, FX_NUM };
struct EffectDef {
	const wchar_t* name;
	int item_num;
	const wchar_t* items[4];
};
const EffectDef EFFECT_DEFS[FX_NUM] = {
	{ L"座標", 3, { L"X", L"Y", L"Z" } },
	{ L"中心座標", 3, { L"X", L"Y", L"Z" } },
	{ L"回転", 3, { L"X", L"Y", L"Z" } },
	{ L"拡大率", 4, { L"X", L"Y", L"Z", L"拡大率" } },
};

// 変更対象のエフェクト1つ分 (EFFECT_HANDLEはコールバックを跨いで使えないためエフェクトIDで保持する)
// エフェクトの種類ごとに、時間変化しないもの(通常)と、移動が設定されたもの(キー指定用)の2つを使い分ける
struct EffectSlot {
	bool prepared = false;	// エフェクトの検索・追加を済ませたか
	int64_t id = 0;			// 0=利用不可
	bool created = false;	// 本プラグインが追加したエフェクトか (キャンセル時等に削除する)
	TrackValue track[4];	// 開始時の値 (キー指定用で追加した場合は移動を設定した後の値)
	std::string applied[4];	// 直近に書き込んだ値 (変化が無い項目は書き込まないために使う)
	bool valid[4] = {};		// 変更できる項目か (通常=単一の数値、キー指定用=キーの数が区間数+1の移動設定)
};

//---------------------------------------------------------------------
//	拡大率の代わりに優先して変更する、オブジェクト本体(エフェクト一覧の先頭)の大きさの項目
//	テキストの「サイズ」等は拡大率と違い、境界がぼけずに拡大される。Alt指定中は使わない
//	[0]=オブジェクトのX [1]=Y [2]=Z [3]=全体。候補の項目名は設定(g_settings.size_items)で変更でき、
//	先頭から探して最初に見つかったものを使う
//---------------------------------------------------------------------
struct SizeItem {
	std::wstring item;				// 見つかった項目名 (空=無し)
	TrackValue track;				// 開始時の値
	std::string applied;			// 直近に書き込んだ値
};

struct TargetObject {
	OBJECT_HANDLE object = nullptr;
	const BaseEffectDef* base = &BASE_EFFECTS[0];	// 変更対象の基本のエフェクトの種類 (カメラ制御等の特例の判定に使う。一番上のエフェクトの場合はGENERIC_BASE)
	std::wstring base_name = STANDARD_DRAW;		// 変更対象の基本のエフェクト名
	const wchar_t* items[ITEM_NUM] = {};		// 各項目に対応する項目名 (nullptr=持たない)
	const wchar_t* center_label = L"中心";		// HUDでの中心の表示名
	bool force_effect = false;					// 操作に対応する項目を持たないため、常にエフェクトを追加して変更する
	TrackValue track[ITEM_NUM];		// 操作開始時の値
	std::string applied[ITEM_NUM];	// 直近に書き込んだ値 (変化が無い項目は書き込まないために使う)
	double now[ITEM_NUM] = {};		// 現在フレームでの値 (ローカル軸の回転行列、変更しない軸の値に使う)
	int key_count = 2;				// 時間変化する項目のキーの数 (区間数+1)
	int prev_key = 0;				// 現在フレームの直前(同じフレームを含む)の始点・中間点のキー番号 (直後のキーはprev_key+1)
	bool has_camera = false;		// カメラ制御の対象か
	double cam_right[3] = {}, cam_down[3] = {}, cam_forward[3] = {};	// カメラから見た画面の右・下・奥方向
	EffectSlot fx[FX_NUM][2];		// [エフェクトの種類][0=通常 1=キー指定用]
	std::wstring size_effect;		// 大きさの項目を持つエフェクト(オブジェクト本体)の名前
	SizeItem size[4];				// 大きさの項目 ([0..2]=X,Y,Z [3]=全体)
};

//---------------------------------------------------------------------
//	操作中の状態
//---------------------------------------------------------------------
enum class Mode { Move, Center, Rotate, Scale };

// 軸指定のビットマスク (0=軸指定無し。Shift+Xは除外指定でAXIS_Y|AXIS_Zになる)
constexpr int AXIS_X = 1, AXIS_Y = 2, AXIS_Z = 4;
constexpr int AXIS_ALL = AXIS_X | AXIS_Y | AXIS_Z;

struct ModalState {
	bool active = false;
	Mode mode = Mode::Move;
	int axes = 0;
	std::vector<TargetObject> targets;
	POINT last_cursor = {};
	double mouse_dx = 0, mouse_dy = 0;	// Shift補正済みのマウス移動量の累積(px)
	double wheel = 0;					// Shift補正済みのホイール回転量の累積(ノッチ。奥へ回すと正)
	bool local = false;					// ローカル軸で移動する (起点キーをもう一度押すと切り替え)
	bool use_effect = false;			// 標準描画ではなくエフェクトを変更する (Altで切り替え)
	bool key_selected = false;			// キー(始点・中間点・終点)を指定して変更するか (←/→で指定、↓で解除)
	int key_offset = 0;					// 指定中のキーの、現在フレームの直前のキーからの相対位置 (0=直前 1=直後)
	UINT trigger_key = 0;				// 起点のコマンドを呼び出したキー (0=不明)
	std::wstring numeric;				// 入力中の数値・式 (例: "10*5", "*2")
	int pending_finish = -1;			// ボタン押下で予約した終了種別 (1=確定 0=キャンセル -1=無し)
};
ModalState g_state;

//---------------------------------------------------------------------
//	汎用プラグイン構造体定義
//---------------------------------------------------------------------
COMMON_PLUGIN_TABLE common_plugin_table = {
	L"Keychain to Transform",
	L"Keychain to Transform version 0.1 By teruyoshi",
};

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
	logger = handle;
}
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) {
	config = handle;
}
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
	return &common_plugin_table;
}

void log_line(const std::wstring& message) {
	if (logger) logger->log(logger, message.c_str());
}

//=======================================================================
//	値の計算
//=======================================================================
//---------------------------------------------------------------------
//	数値入力の式の評価 (+,-,*,/ の四則演算。*,/を優先し、単項の+,-に対応)
//---------------------------------------------------------------------
bool parse_expr(const std::wstring& s, size_t& pos, double& out);

bool parse_factor(const std::wstring& s, size_t& pos, double& out) {
	if (pos < s.size() && (s[pos] == L'+' || s[pos] == L'-')) {
		bool negative = (s[pos++] == L'-');
		if (!parse_factor(s, pos, out)) return false;
		if (negative) out = -out;
		return true;
	}
	const wchar_t* begin = s.c_str() + pos;
	wchar_t* end = nullptr;
	out = wcstod(begin, &end);
	if (end == begin) return false;
	pos += end - begin;
	return true;
}

bool parse_term(const std::wstring& s, size_t& pos, double& out) {
	if (!parse_factor(s, pos, out)) return false;
	while (pos < s.size() && (s[pos] == L'*' || s[pos] == L'/')) {
		wchar_t op = s[pos++];
		double rhs;
		if (!parse_factor(s, pos, rhs)) return false;
		if (op == L'*') out *= rhs;
		else if (rhs == 0) return false;
		else out /= rhs;
	}
	return true;
}

bool parse_expr(const std::wstring& s, size_t& pos, double& out) {
	if (!parse_term(s, pos, out)) return false;
	while (pos < s.size() && (s[pos] == L'+' || s[pos] == L'-')) {
		wchar_t op = s[pos++];
		double rhs;
		if (!parse_term(s, pos, rhs)) return false;
		out = (op == L'+') ? out + rhs : out - rhs;
	}
	return true;
}

// 入力途中の式も評価できるよう、末尾の演算子は無視する ("10*" → 10)。
// ただし末尾に「-」が残っている場合は、それより前の式の計算結果の符号を反転する (Blenderと同じ。"5-" → -5、"20/5-" → -4)
bool evaluate_expression(std::wstring s, double& out) {
	bool negate = (!s.empty() && s.back() == L'-');
	while (!s.empty() && wcschr(L"+-*/", s.back())) s.pop_back();
	if (s.empty()) return false;
	size_t pos = 0;
	if (!parse_expr(s, pos, out) || pos != s.size()) return false;
	if (negate) out = -out;
	return true;
}

// 数値入力の種類。先頭が=なら上書き、*か/なら元の値への乗除算、それ以外は変化量の加算(拡大は倍率)
enum class NumericOp { Add, Multiply, Set };

// 数値入力の解釈結果
struct NumericInput {
	bool active = false;	// 数値入力中か (入力中はマウス移動を無視する)
	bool valid = false;		// 式として評価できたか (無効な間は値を変えない)
	NumericOp op = NumericOp::Add;
	double value = 0;		// Multiplyの場合、/は逆数にして保持する
};

NumericInput get_numeric_input() {
	NumericInput n;
	const std::wstring& s = g_state.numeric;
	if (s.empty()) return n;
	n.active = true;
	if (s[0] == L'=') {
		n.op = NumericOp::Set;
		if (!evaluate_expression(s.substr(1), n.value)) return n;
	} else if (s[0] == L'*' || s[0] == L'/') {
		n.op = NumericOp::Multiply;
		double v;
		if (!evaluate_expression(s.substr(1), v)) return n;
		if (s[0] == L'/') {
			if (v == 0) return n;
			v = 1.0 / v;
		}
		n.value = v;
	} else if (!evaluate_expression(s, n.value)) {
		return n;
	}
	n.valid = true;
	return n;
}

// 数値入力を1項目に適用する (移動・回転用)
double apply_numeric(double original, const NumericInput& n) {
	if (!n.valid) return original;
	switch (n.op) {
		case NumericOp::Multiply: return original * n.value;
		case NumericOp::Set: return n.value;
		default: return original + n.value;
	}
}

bool axis_on(int a) {
	return (g_state.axes & (1 << a)) != 0;
}

// 移動・中心移動・回転で変更するX,Y,Z(または各軸回転)の先頭の項目
int base_item() {
	switch (g_state.mode) {
		case Mode::Center: return ITEM_CX;
		case Mode::Rotate: return ITEM_RX;
		default: return ITEM_X;
	}
}

// 数値入力で変更する軸。軸指定無しは3軸全て
int numeric_axes() {
	return g_state.axes ? g_state.axes : AXIS_ALL;
}

// 軸a(0=X,1=Y,2=Z)に割り当てるマウス操作量 (マウスはpx、ホイールはノッチ)。割り当てが無い軸は0
//   移動・中心移動:
//     軸指定無し : X=横, Y=縦, Z=ホイール(奥へ回すと奥へ)
//     1軸        : X=横, Y=縦, Z=縦(上へ動かすと奥へ)
//     2軸(Shift) : XY: X=横,Y=縦 / XZ: X=横,Z=縦 / YZ: Z=横,Y=縦
//   回転:
//     軸指定無し : Z軸回転=横 (画面平面の回転)。カメラ制御(camera=true)はX軸回転=縦, Y軸回転=横, Z軸回転=ホイール
//     1軸        : X軸回転=縦, Y/Z軸回転=横
//     2軸(Shift) : XY: X=縦,Y=横 / XZ: X=縦,Z=横 / YZ: Y=横,Z=縦
double mouse_amount(int a, bool camera) {
	double dx = g_state.mouse_dx, dy = g_state.mouse_dy;
	double camera_dy = g_settings.invert_camera_pitch ? -dy : dy;	// カメラ制御の上下の向き(設定で反転)
	int m = g_state.axes;
	bool rotate = (g_state.mode == Mode::Rotate);
	if (m == 0) {
		if (rotate && camera) return (a == 0) ? camera_dy : (a == 1) ? dx : g_state.wheel;
		if (rotate) return (a == 2) ? dx : 0;
		return (a == 0) ? dx : (a == 1) ? dy : g_state.wheel;
	}
	if (!axis_on(a)) return 0;
	bool single = (m == AXIS_X || m == AXIS_Y || m == AXIS_Z);
	if (rotate) {
		if (a == 0) return camera ? camera_dy : dy;
		if (a == 1) return dx;
		return (single || (m & AXIS_X)) ? dx : dy;
	}
	if (a == 0) return dx;
	if (a == 1) return dy;
	return (single || (m & AXIS_X)) ? -dy : dx;
}

// 軸aのマウスによる変化量 (移動・中心移動は座標、回転は度)。cameraはカメラ制御の回転か
double mouse_delta(int a, bool camera) {
	bool wheel = (g_state.axes == 0 && a == 2);
	if (g_state.mode == Mode::Rotate) return mouse_amount(a, camera) * ((wheel && camera) ? g_settings.rotate_per_wheel : g_settings.rotate_per_px);
	return mouse_amount(a, camera) * (wheel ? g_settings.move_per_wheel : g_settings.move_per_px);
}

// マウスによる拡大の倍率
double mouse_scale_factor() {
	return std::max(0.0, 1.0 + g_state.mouse_dx * g_settings.scale_per_px);
}

// 拡大率(100基準)を計算する。数値入力は「2」も「*2」も2倍、「=150」は150で上書き
double apply_scale(double original) {
	NumericInput n = get_numeric_input();
	if (!n.active) return std::max(0.0, original * mouse_scale_factor());
	if (!n.valid) return original;
	return std::max(0.0, n.op == NumericOp::Set ? n.value : original * n.value);
}

// 視野角(カメラ制御の拡大)を計算する。視野角を狭めるほど大きく写るため、拡大の倍率で割る。「=60」は60で上書き
double apply_inverse_scale(double original) {
	NumericInput n = get_numeric_input();
	double factor;
	if (!n.active) factor = mouse_scale_factor();
	else if (!n.valid) return original;
	else if (n.op == NumericOp::Set) return std::max(0.0, n.value);
	else factor = n.value;
	return original / std::max(factor, 0.01);
}

// 現在のモードに対応するエフェクト
int mode_effect_of(Mode mode) {
	switch (mode) {
		case Mode::Center: return FX_CENTER;
		case Mode::Rotate: return FX_ROTATE;
		case Mode::Scale: return FX_SCALE;
		default: return FX_MOVE;
	}
}
int mode_effect() {
	return mode_effect_of(g_state.mode);
}

// 現在のモードで変更する標準描画の項目の範囲
void mode_items(int& first, int& count) {
	first = (g_state.mode == Mode::Scale) ? ITEM_ZOOM : base_item();
	count = (g_state.mode == Mode::Scale) ? 1 : 3;
}

// 項目がキー指定で変更できるか (移動が設定され、値の数が区間数+1)
bool keyable(const TrackValue& tv, int key_count) {
	return tv.valid && tv.moving && (int)tv.keys.size() == key_count;
}

// 現在のモードで変更する標準描画の項目のいずれかが時間変化するか (移動方法の設定や式がある)
bool has_varying_item(const TargetObject& t) {
	int first, count;
	mode_items(first, count);
	for (int i = first; i < first + count; i++) {
		if (t.track[i].valid && !t.track[i].plain()) return true;
	}
	return false;
}

// 指定中のキー番号
int selected_key(const TargetObject& t) {
	return std::clamp(t.prev_key + g_state.key_offset, 0, t.key_count - 1);
}

// 拡大の全体の拡大率を表すビット (軸マスクと同じ変数で扱うため、X,Y,Zの次のビットを使う)
constexpr int ITEM_BIT_ZOOM = 8;

// 現在の操作で値が変わる軸 (拡大の軸指定無しは全体の拡大率)
int affected_axes() {
	if (g_state.mode == Mode::Scale) return g_state.axes ? g_state.axes : ITEM_BIT_ZOOM;
	if (get_numeric_input().active) return numeric_axes();
	return g_state.axes ? g_state.axes : AXIS_ALL;
}

int scale_object_axes(const TargetObject& t);

// キー指定中に、標準描画に移動が設定されていないためエフェクト側で変更する軸
int unkeyable_axes(const TargetObject& t) {
	int affected = (g_state.mode == Mode::Scale) ? scale_object_axes(t) : affected_axes();
	if (g_state.mode == Mode::Scale) {
		if (affected != ITEM_BIT_ZOOM) return affected; // 軸指定の拡大は常にエフェクト
		const TrackValue& tv = t.track[ITEM_ZOOM];
		return (tv.valid && !keyable(tv, t.key_count)) ? ITEM_BIT_ZOOM : 0;
	}
	int m = 0;
	for (int a = 0; a < 3; a++) {
		const TrackValue& tv = t.track[base_item() + a];
		if ((affected & (1 << a)) && tv.valid && !keyable(tv, t.key_count)) m |= 1 << a;
	}
	return m;
}

// 値を書き換える対象のエフェクト
struct EffectTarget {
	int kind = -1;		// -1=エフェクトを使わない
	bool keyed = false;	// キー指定用(移動を設定した)エフェクトか
	int mask = 0;		// エフェクト側で変更する軸 (拡大の全体の拡大率はITEM_BIT_ZOOM)。それ以外の軸は標準描画を変更する
};

//   Alt指定中: モードに対応するエフェクトで全て変更する
//   中心を持たない基本のエフェクト(グループ制御)の中心移動: 中心座標エフェクト
//   拡大の軸指定: 標準描画には軸別の拡大率が無いため、拡大率エフェクト
//   キー指定無しで項目が時間変化する: 対応するエフェクト(通常)を追加して全て変更する
//   キー指定中: 移動が設定されていない軸のみ、移動を設定した対応するエフェクト(キー指定用)を追加して変更する
bool camera_rotating(const TargetObject& t);

// 大きさの項目iが今の状態で変更できるか (キー指定無しは単一の数値、キー指定中は移動が設定されていること)
bool size_usable(const TargetObject& t, int i) {
	const SizeItem& size = t.size[i];
	if (size.item.empty()) return false;
	return g_state.key_selected ? keyable(size.track, t.key_count) : size.track.plain();
}

// 拡大で、拡大率の代わりに大きさの項目で変更する軸 (全体はITEM_BIT_ZOOM)。Alt指定中・カメラ制御では使わない
int size_axes(const TargetObject& t) {
	if (g_state.mode != Mode::Scale || g_state.use_effect || t.base->camera) return 0;
	if (g_state.axes == 0) return size_usable(t, 3) ? ITEM_BIT_ZOOM : 0;
	int m = 0, axes = scale_object_axes(t);
	for (int i = 0; i < 3; i++) if ((axes & (1 << i)) && size_usable(t, i)) m |= 1 << i;
	return m;
}

EffectTarget effect_target(const TargetObject& t) {
	EffectTarget e;
	if (camera_rotating(t)) return e; // カメラ制御の回転ではAltは目標点中心の回転に使い、エフェクトは使わない
	if (g_state.mode == Mode::Scale && t.base->camera) return e; // カメラ制御には拡大率エフェクトを追加できない
	// 操作に対応する項目を持たない場合は、常にエフェクトで全て変更する (操作開始時に追加できたものだけが対象になっている)
	if (t.force_effect) {
		e.keyed = g_state.key_selected;
		e.kind = mode_effect();
		e.mask = (g_state.mode == Mode::Scale) ? scale_object_axes(t) : AXIS_ALL;
		return e;
	}
	// 大きさの項目で変更する軸は、拡大率エフェクトを使わない
	int sized = size_axes(t);
	if (sized == ITEM_BIT_ZOOM) return e;
	int all = (g_state.mode == Mode::Scale) ? (scale_object_axes(t) & ~sized) : AXIS_ALL;
	if (g_state.mode == Mode::Scale && g_state.axes != 0 && all == 0) return e;
	e.keyed = g_state.key_selected;
	bool forced = (g_state.mode == Mode::Center && t.base->center_by_effect) || (g_state.mode == Mode::Scale && g_state.axes != 0);
	if (g_state.use_effect || forced || (!e.keyed && has_varying_item(t))) {
		e.kind = mode_effect();
		e.mask = all;
	} else if (e.keyed) {
		e.mask = unkeyable_axes(t) & ~sized;
		if (e.mask) e.kind = mode_effect();
	}
	return e;
}

// 標準描画の項目iで変更するキー番号 (-1=変更しない)
//   キー指定無し: 単一の数値なら0
//   キー指定中: 移動が設定されていれば指定中のキー
int edit_key(const TargetObject& t, int i) {
	const TrackValue& tv = t.track[i];
	if (!g_state.key_selected) return tv.plain() ? 0 : -1;
	return keyable(tv, t.key_count) ? selected_key(t) : -1;
}

bool local_supported();

// ローカル軸で操作中か (起点キーや軸キーをもう一度押すと切り替わる)
bool local_active() {
	return g_state.local && local_supported();
}

// カメラの向きを基準にマウス操作するか (カメラ制御の対象で、軸指定無し・ローカル軸でないマウスによる操作)
//   移動・中心移動: マウスの横・縦・ホイールをカメラから見た右・下・奥方向に割り当てる
//   回転          : 画面のZ軸ではなくカメラの視線を軸に回す
//   拡大は軸指定無しでは全体の一様な拡大なので、カメラの向きに依存しない
bool camera_active(const TargetObject& t) {
	bool mode = g_state.mode == Mode::Move || g_state.mode == Mode::Rotate || (g_state.mode == Mode::Center && !t.base->camera);
	return t.has_camera && mode && g_state.axes == 0 && !local_active() && !get_numeric_input().active;
}

// 標準描画の回転(度)からオブジェクトのローカル軸→画面の回転行列を作る。
// 座標系はX=右, Y=下, Z=奥。Z軸回転は画面上で時計回りが正 (Y軸が下向きのため標準の回転行列で時計回りになる)。
// X→Y→Zの順に適用する(R=Rz*Ry*Rx)。合成順序はドキュメントに記載が無いが、実機で確認済み
void rotation_matrix(double rx_deg, double ry_deg, double rz_deg, double r[3][3]) {
	const double to_rad = 3.14159265358979323846 / 180.0;
	double cx = cos(rx_deg * to_rad), sx = sin(rx_deg * to_rad);
	double cy = cos(ry_deg * to_rad), sy = sin(ry_deg * to_rad);
	double cz = cos(rz_deg * to_rad), sz = sin(rz_deg * to_rad);
	double rx[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
	double ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
	double rz[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
	double ryx[3][3];
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
		ryx[i][j] = 0;
		for (int k = 0; k < 3; k++) ryx[i][j] += ry[i][k] * rx[k][j];
	}
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
		r[i][j] = 0;
		for (int k = 0; k < 3; k++) r[i][j] += rz[i][k] * ryx[k][j];
	}
}

// 操作する座標系と、値を保存している座標系が異なるか。異なる場合は回転行列mを返す
// (値pを作業座標系に変換するにはmの転置を掛け、戻すにはmを掛ける)
//   移動のローカル軸  : 値は画面の軸、作業はオブジェクトの軸 (m=R)
//   中心移動のグローバル軸: 値はオブジェクトの軸(中心X/Y/Zは回転前の座標)、作業は画面の軸 (m=Rの転置)
bool work_frame(const TargetObject& t, double m[3][3]) {
	bool local = local_active();
	bool move_local = (g_state.mode == Mode::Move && local);
	bool center_global = (g_state.mode == Mode::Center && !local && !t.base->camera);
	if (!move_local && !center_global) return false;
	double r[3][3];
	rotation_matrix(t.now[ITEM_RX], t.now[ITEM_RY], t.now[ITEM_RZ], r);
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = move_local ? r[i][j] : r[j][i];
	return true;
}

// 移動・中心移動・回転のX,Y,Z(または各軸回転)の3値を、マウス・数値入力に応じて計算する。
// カメラ制御の対象では、マウスの横・縦・ホイールをカメラから見た右・下・奥方向に割り当てる。
// 操作する座標系と値の座標系が異なる場合(work_frame)は、作業座標系に変換してから同じ計算を行い、元に戻す
void compute_xyz(const TargetObject& t, const double in[3], double out[3]) {
	if (camera_active(t) && g_state.mode != Mode::Rotate) {
		double mx = g_state.mouse_dx * g_settings.move_per_px, my = g_state.mouse_dy * g_settings.move_per_px, mz = g_state.wheel * g_settings.move_per_wheel;
		double d[3];
		for (int i = 0; i < 3; i++) d[i] = t.cam_right[i] * mx + t.cam_down[i] * my + t.cam_forward[i] * mz;
		// 中心X/Y/Zはオブジェクトの軸(回転前)の値なので、画面の軸での移動量をオブジェクトの軸に変換する (回転行列の転置を掛ける)
		if (g_state.mode == Mode::Center) {
			double r[3][3], w[3] = { d[0], d[1], d[2] };
			rotation_matrix(t.now[ITEM_RX], t.now[ITEM_RY], t.now[ITEM_RZ], r);
			for (int i = 0; i < 3; i++) d[i] = r[0][i] * w[0] + r[1][i] * w[1] + r[2][i] * w[2];
		}
		for (int i = 0; i < 3; i++) out[i] = in[i] + d[i];
		return;
	}
	NumericInput n = get_numeric_input();
	double p[3] = { in[0], in[1], in[2] };
	double m[3][3];
	bool convert = work_frame(t, m);
	if (convert) {
		for (int i = 0; i < 3; i++) p[i] = m[0][i] * in[0] + m[1][i] * in[1] + m[2][i] * in[2]; // 転置を掛ける
	}
	for (int a = 0; a < 3; a++) {
		if (!n.active) p[a] += mouse_delta(a, camera_rotating(t));
		else if (numeric_axes() & (1 << a)) p[a] = apply_numeric(p[a], n);
	}
	for (int i = 0; i < 3; i++) out[i] = convert ? m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] : p[i];
}

// 角度(度)をrefに最も近い同値の角度にする (360度の整数倍を足し引きする)
double nearest_angle(double deg, double ref) {
	return deg + 360.0 * std::round((ref - deg) / 360.0);
}

// 回転行列 r (=Rz*Ry*Rx) から各軸回転の角度(度)を求める。解が2通りあるため、元の角度refに近い方を選ぶ。
// Y軸回転が±90度付近(ジンバルロック)ではX軸回転をrefのまま固定してZ軸回転を求める
void euler_from_matrix(const double r[3][3], const double ref[3], double out[3]) {
	const double to_deg = 180.0 / 3.14159265358979323846;
	double sy = std::clamp(-r[2][0], -1.0, 1.0);
	double candidates[2][3];
	int count;
	if (std::fabs(sy) < 0.9999999) {
		double ry = asin(sy);
		candidates[0][0] = atan2(r[2][1], r[2][2]) * to_deg;
		candidates[0][1] = ry * to_deg;
		candidates[0][2] = atan2(r[1][0], r[0][0]) * to_deg;
		candidates[1][0] = atan2(-r[2][1], -r[2][2]) * to_deg;
		candidates[1][1] = 180.0 - ry * to_deg;
		candidates[1][2] = atan2(-r[1][0], -r[0][0]) * to_deg;
		count = 2;
	} else {
		// sy=1: 行0,1が (0, sin(rx-rz), cos(rx-rz)), (0, cos(rx-rz), -sin(rx-rz))
		// sy=-1: 行0,1が (0, -sin(rx+rz), -cos(rx+rz)), (0, cos(rx+rz), -sin(rx+rz))
		double rx = ref[0];
		candidates[0][0] = rx;
		if (sy > 0) {
			candidates[0][1] = 90;
			candidates[0][2] = rx - atan2(r[0][1], r[1][1]) * to_deg;
		} else {
			candidates[0][1] = -90;
			candidates[0][2] = atan2(-r[0][1], r[1][1]) * to_deg - rx;
		}
		count = 1;
	}
	double best = -1;
	for (int c = 0; c < count; c++) {
		double v[3], dist = 0;
		for (int a = 0; a < 3; a++) {
			v[a] = nearest_angle(candidates[c][a], ref[a]);
			dist += (v[a] - ref[a]) * (v[a] - ref[a]);
		}
		if (best < 0 || dist < best) {
			best = dist;
			for (int a = 0; a < 3; a++) out[a] = v[a];
		}
	}
}

// 標準描画の回転。マウス・数値入力(加算)による各軸の回転量を回転行列で合成し、各軸回転の角度に戻す。
//   グローバル: 画面の軸まわりに回す (元の回転の外側から掛ける)
//   ローカル  : オブジェクト自身の軸まわりに回す (元の回転の内側から掛ける)
// 「=」「*」の数値入力は角度そのものへの指定なので、各軸回転の値を直接変更する
void compute_rotation(const TargetObject& t, const double in[3], double out[3]) {
	NumericInput n = get_numeric_input();
	if (n.active && n.op != NumericOp::Add) {
		compute_xyz(t, in, out);
		return;
	}
	double zero[3] = { 0, 0, 0 }, delta[3];
	compute_xyz(t, zero, delta);
	double r[3][3];
	rotation_matrix(in[0], in[1], in[2], r);
	bool local = local_active();
	bool camera = camera_active(t);
	for (int a = 0; a < 3; a++) {
		if (delta[a] == 0) continue;
		double d[3][3], result[3][3];
		if (camera && a == 2) {
			// カメラ制御の対象: 画面内の回転(Z軸回転)はカメラの視線を軸に回す (ロドリゲスの回転公式の行列)
			const double rad = delta[a] * 3.14159265358979323846 / 180.0;
			double c = cos(rad), sn = sin(rad);
			const double* f = t.cam_forward;
			double k[3][3] = { { 0, -f[2], f[1] }, { f[2], 0, -f[0] }, { -f[1], f[0], 0 } };
			for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) d[i][j] = (i == j ? c : 0) + (1 - c) * f[i] * f[j] + sn * k[i][j];
		} else {
			rotation_matrix(a == 0 ? delta[a] : 0, a == 1 ? delta[a] : 0, a == 2 ? delta[a] : 0, d);
		}
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
			result[i][j] = 0;
			for (int k = 0; k < 3; k++) result[i][j] += local ? r[i][k] * d[k][j] : d[i][k] * r[k][j];
		}
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i][j] = result[i][j];
	}
	euler_from_matrix(r, in, out);
}

// 軸指定の拡大で、オブジェクト自身の各軸に掛ける倍率の指数 (拡大率エフェクトのX/Y/Zはオブジェクトの軸方向に伸縮するため)。
// 倍率fに対し、オブジェクトの軸iはf^weight[i]倍にする。
//   ローカル  : 指定した軸は1、それ以外は0
//   グローバル: 指定した画面の軸それぞれについて、オブジェクトの軸iとの方向余弦の2乗の和
//               (例: Z軸回転45度でS→X→2なら、オブジェクトのX,Yはどちらも0.5で√2倍。回転が90度単位なら1つの軸だけが1になり正確。
//                斜めの場合、画面の軸方向への伸縮はせん断になり拡大率エフェクトでは表せないため近似。3軸とも指定した場合は各軸1になり全体の拡大と一致)
//   上書き(=150): 倍率として分配できないため、指定した画面の軸それぞれに最も近い向きのオブジェクトの軸を1にする
void scale_weights(const TargetObject& t, double weight[3]) {
	for (int i = 0; i < 3; i++) weight[i] = 0;
	if (local_active()) {
		for (int i = 0; i < 3; i++) if (axis_on(i)) weight[i] = 1;
		return;
	}
	double r[3][3];
	rotation_matrix(t.now[ITEM_RX], t.now[ITEM_RY], t.now[ITEM_RZ], r);
	NumericInput n = get_numeric_input();
	bool set = n.active && n.op == NumericOp::Set;
	for (int w = 0; w < 3; w++) {
		if (!axis_on(w)) continue;
		// オブジェクトの軸iの画面での向きは回転行列の列iなので、画面の軸wとの方向余弦はr[w][i]
		if (set) {
			int best = 0;
			for (int i = 1; i < 3; i++) if (std::fabs(r[w][i]) > std::fabs(r[w][best])) best = i;
			weight[best] = 1;
		} else {
			for (int i = 0; i < 3; i++) weight[i] += r[w][i] * r[w][i];
		}
	}
}

// 軸指定の拡大で変更する、オブジェクト自身の軸 (軸指定無しは全体の拡大率 ITEM_BIT_ZOOM)
int scale_object_axes(const TargetObject& t) {
	if (g_state.axes == 0) return ITEM_BIT_ZOOM;
	double weight[3];
	scale_weights(t, weight);
	int m = 0;
	for (int i = 0; i < 3; i++) if (weight[i] > 1e-6) m |= 1 << i;
	return m;
}

// 軸指定の拡大で、オブジェクトの軸の拡大率(100基準)を計算する (weightはscale_weightsの値)
double apply_axis_scale(double original, double weight) {
	NumericInput n = get_numeric_input();
	if (n.active && n.op == NumericOp::Set) return apply_scale(original);
	double factor = !n.active ? mouse_scale_factor() : n.valid ? std::max(0.0, n.value) : 1.0;
	return std::max(0.0, original * std::pow(factor, weight));
}

int size_axes(const TargetObject& t);

// 大きさの項目の設定値(文字列)。拡大率と同じ計算(全体はapply_scale、軸指定はapply_axis_scale)で変更する
void compute_size_values(const TargetObject& t, std::string out[4]) {
	for (int i = 0; i < 4; i++) out[i] = t.size[i].track.raw;
	int m = size_axes(t);
	if (!m) return;
	int k = g_state.key_selected ? selected_key(t) : 0;
	auto put = [&](int i, double v) {
		const TrackValue& tv = t.size[i].track;
		out[i] = tv.text_with(k, format_value(v, tv.keys[k]));
	};
	if (m == ITEM_BIT_ZOOM) {
		put(3, apply_scale(t.size[3].track.key(k)));
		return;
	}
	double weight[3];
	scale_weights(t, weight);
	for (int i = 0; i < 3; i++) {
		if (m & (1 << i)) put(i, apply_axis_scale(t.size[i].track.key(k), weight[i]));
	}
}

bool normalize(double v[3]);
void cross(const double a[3], const double b[3], double out[3]);

// カメラ制御の回転中か (カメラ制御の回転は専用の計算を行い、エフェクトは使わない)
bool camera_rotating(const TargetObject& t) {
	return g_state.mode == Mode::Rotate && t.base->camera;
}

// カメラの視線方向forward(正規化不要)と傾き(度)から、画面の右・下方向の単位ベクトルを求める。
// 座標系はX=右,Y=下,Z=奥。上方向は-Y。真上・真下を向いている場合は画面のX軸を右方向の基準とする。
// 傾きは視線方向を軸に右・下方向を回転させる (正の向きは実機で確認済み)
void camera_basis(const double forward_in[3], double tilt_deg, double right_out[3], double down_out[3]) {
	double forward[3] = { forward_in[0], forward_in[1], forward_in[2] };
	if (!normalize(forward)) { forward[0] = 0; forward[1] = 0; forward[2] = 1; }
	double up[3] = { 0, -1, 0 };
	double right[3], down[3];
	cross(forward, up, right);
	if (!normalize(right)) { right[0] = 1; right[1] = 0; right[2] = 0; }
	cross(forward, right, down);
	normalize(down);
	const double rad = tilt_deg * 3.14159265358979323846 / 180.0;
	double c = cos(rad), s = sin(rad);
	for (int i = 0; i < 3; i++) {
		right_out[i] = right[i] * c - down[i] * s;
		down_out[i] = down[i] * c + right[i] * s;
	}
}

// 視線方向forwardのカメラで、画面の右方向がrightになる傾き(度)。camera_basisの逆算
double camera_tilt_from(const double forward[3], const double right[3]) {
	double right0[3], down0[3];
	camera_basis(forward, 0, right0, down0);
	double c = right[0] * right0[0] + right[1] * right0[1] + right[2] * right0[2];
	double s = -(right[0] * down0[0] + right[1] * down0[1] + right[2] * down0[2]);
	return atan2(s, c) * 180.0 / 3.14159265358979323846;
}

// ベクトルvを単位ベクトルaxisまわりにdeg度回転する (ロドリゲスの回転公式)
void rotate_around(double v[3], const double axis[3], double deg) {
	const double rad = deg * 3.14159265358979323846 / 180.0;
	double c = cos(rad), s = sin(rad);
	double kxv[3], kv = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
	cross(axis, v, kxv);
	for (int i = 0; i < 3; i++) v[i] = v[i] * c + kxv[i] * s + axis[i] * kv * (1 - c);
}

void multiply(const double r[3][3], double v[3]) {
	double w[3] = { v[0], v[1], v[2] };
	for (int i = 0; i < 3; i++) v[i] = r[i][0] * w[0] + r[i][1] * w[1] + r[i][2] * w[2];
}

// カメラ制御の回転。回転の向き、マウス・数値入力の割り当ては標準描画の回転と同じ (開始時を0度とした回転量として扱う)
//   R→X/Y/Z (グローバル): 画面のX/Y/Z軸まわりにカメラ全体(視線方向と画面の右方向)を回転させ、目標点と傾きを求め直す
//   R→R→X/Y/Z (ローカル): Y軸回転(左右の向き)は鉛直軸まわり、X軸回転(上下の向き)はカメラの右方向まわりに回す。
//                         Z軸回転はグローバルと同じ向きになるよう傾きから引く
//   Alt指定中: カメラの位置ではなく目標点を中心に回し、目標点の代わりにカメラの位置を変更する
void rotate_camera(const TargetObject& t, std::string out[ITEM_NUM]) {
	double zero[3] = { 0, 0, 0 }, angle[3];
	compute_xyz(t, zero, angle);
	bool local = g_state.local;
	bool orbit = g_state.use_effect;
	if (angle[0] == 0 && angle[1] == 0 && angle[2] == 0) return;

	int kp[3], kt[3];
	double pos[3], target[3];
	for (int a = 0; a < 3; a++) {
		kp[a] = edit_key(t, ITEM_X + a);
		pos[a] = (kp[a] >= 0) ? t.track[ITEM_X + a].key(kp[a]) : t.now[ITEM_X + a];
		kt[a] = edit_key(t, ITEM_CX + a);
		target[a] = (kt[a] >= 0) ? t.track[ITEM_CX + a].key(kt[a]) : t.now[ITEM_CX + a];
	}
	int k_tilt = edit_key(t, ITEM_RZ);
	double tilt = (k_tilt >= 0) ? t.track[ITEM_RZ].key(k_tilt) : t.now[ITEM_RZ];

	double d[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
	if (local) {
		// 視線方向と一緒に、傾きを反映した画面の右方向も回し、回転後の傾きを求め直す
		// (上方向は常に-Y基準なので、上下の向きが真上・真下を越えた場合は傾きが180度変わる。
		//  これを反映しないと、真上・真下を越えたところで向きが跳ね返ったように見える)
		double right[3], down[3];
		camera_basis(d, tilt, right, down);
		double ry[3][3];
		rotation_matrix(0, angle[1], 0, ry);
		multiply(ry, d);
		multiply(ry, right);
		// 上下の向きの回転軸は、傾きを含まない水平な右方向
		double axis[3], axis_down[3];
		camera_basis(d, 0, axis, axis_down);
		rotate_around(d, axis, angle[0]);
		rotate_around(right, axis, angle[0]);
		double diff = fmod(camera_tilt_from(d, right) - tilt, 360.0);
		if (diff > 180) diff -= 360;
		if (diff < -180) diff += 360;
		// Z軸回転(ロール)はグローバルと同じく、カメラを物体と同じ向きに回す(傾きは減る方向)
		if (k_tilt >= 0) out[ITEM_RZ] = t.track[ITEM_RZ].text_with(k_tilt, format_value(tilt + diff - angle[2], t.track[ITEM_RZ].keys[k_tilt]));
	} else {
		double right[3], down[3];
		camera_basis(d, tilt, right, down);
		double r[3][3];
		rotation_matrix(angle[0], angle[1], angle[2], r);
		multiply(r, d);
		multiply(r, right);
		// 傾きは元の値からの差分で更新する (-180～180度に丸めた値で置き換えると、元の傾きが範囲外の場合に値が飛ぶため)
		double diff = fmod(camera_tilt_from(d, right) - tilt, 360.0);
		if (diff > 180) diff -= 360;
		if (diff < -180) diff += 360;
		if (k_tilt >= 0) out[ITEM_RZ] = t.track[ITEM_RZ].text_with(k_tilt, format_value(tilt + diff, t.track[ITEM_RZ].keys[k_tilt]));
	}

	// 目標点を動かす(カメラの位置中心) / カメラの位置を動かす(目標点中心)
	int first = orbit ? ITEM_X : ITEM_CX;
	const int* k = orbit ? kp : kt;
	for (int a = 0; a < 3; a++) {
		if (k[a] < 0) continue;
		double value = orbit ? target[a] - d[a] : pos[a] + d[a];
		const TrackValue& tv = t.track[first + a];
		out[first + a] = tv.text_with(k[a], format_value(value, tv.keys[k[a]]));
	}
}

// 標準描画の各項目の設定値(文字列)
void compute_values(const TargetObject& t, std::string out[ITEM_NUM]) {
	for (int i = 0; i < ITEM_NUM; i++) out[i] = t.track[i].raw;
	// エフェクト側で変更する軸は、標準描画を開始時の値のままにする
	int effect_mask = effect_target(t).mask;
	if (g_state.mode == Mode::Scale) {
		// 標準描画の拡大率は軸指定無しの場合のみ変更する (軸指定は拡大率エフェクトか大きさの項目、カメラ制御は何もしない)。
		// 全体をサイズで変更する場合も拡大率は変えない
		if (effect_mask || g_state.axes != 0 || size_axes(t) == ITEM_BIT_ZOOM) return;
		const TrackValue& tv = t.track[ITEM_ZOOM];
		int k = edit_key(t, ITEM_ZOOM);
		double value = (k < 0) ? 0 : t.base->camera ? apply_inverse_scale(tv.key(k)) : apply_scale(tv.key(k));
		if (k >= 0) out[ITEM_ZOOM] = tv.text_with(k, format_value(value, tv.keys[k]));
		return;
	}
	int base = base_item();
	int k[3];
	double in[3], res[3];
	for (int a = 0; a < 3; a++) {
		k[a] = (effect_mask & (1 << a)) ? -1 : edit_key(t, base + a);
		in[a] = (k[a] >= 0) ? t.track[base + a].key(k[a]) : t.now[base + a];
	}
	// 標準描画の回転は3軸とも変更できる場合のみ回転行列で合成する (一部の軸がエフェクト側の場合は各軸の値を直接変更する)
	if (g_state.mode == Mode::Rotate && !camera_rotating(t) && k[0] >= 0 && k[1] >= 0 && k[2] >= 0) compute_rotation(t, in, res);
	else compute_xyz(t, in, res);
	for (int a = 0; a < 3; a++) {
		if (k[a] < 0) continue;
		const TrackValue& tv = t.track[base + a];
		out[base + a] = tv.text_with(k[a], format_value(res[a], tv.keys[k[a]]));
	}
	if (camera_rotating(t)) rotate_camera(t, out);
}

// エフェクトの各項目の設定値(文字列)。現在書き換え対象のエフェクトでなければ開始時の値のまま
void compute_effect_values(const TargetObject& t, int kind, bool keyed, std::string out[4]) {
	const EffectSlot& slot = t.fx[kind][keyed];
	for (int i = 0; i < 4; i++) out[i] = slot.track[i].raw;
	EffectTarget e = effect_target(t);
	if (kind != e.kind || keyed != e.keyed) return;

	int k = keyed ? selected_key(t) : 0;
	auto value = [&](int i) { return slot.valid[i] ? slot.track[i].key(k) : 0.0; };
	// エフェクト側で変更する軸(e.mask)のみ書き換える
	auto put = [&](int i, double v) {
		int bit = (kind == FX_SCALE && i == 3) ? ITEM_BIT_ZOOM : (1 << i);
		if (slot.valid[i] && (e.mask & bit)) out[i] = slot.track[i].text_with(k, format_value(v, slot.track[i].keys[k]));
	};
	if (kind == FX_SCALE) {
		put(3, apply_scale(value(3)));
		double weight[3];
		scale_weights(t, weight);
		for (int a = 0; a < 3; a++) put(a, apply_axis_scale(value(a), weight[a]));
		return;
	}
	double in[3] = { value(0), value(1), value(2) }, res[3];
	compute_xyz(t, in, res);
	for (int a = 0; a < 3; a++) put(a, res[a]);
}

//=======================================================================
//	値の書き込み
//=======================================================================
enum class Finish { None, Commit, Cancel };

struct Write {
	OBJECT_HANDLE object;
	const wchar_t* base_effect;	// effect_id=0の場合に書き込む基本のエフェクト名
	int64_t effect_id;	// 0=標準描画
	const wchar_t* item;
	std::string value;
};

// 直近の書き込み値から変化していれば書き込みリストに追加する
void add_write(std::vector<Write>& writes, const TargetObject& t, int64_t effect_id, const wchar_t* item, const std::string& value, std::string& applied) {
	if (value == applied) return;
	writes.push_back({ t.object, t.base_name.c_str(), effect_id, item, value });
	applied = value;
}

EFFECT_HANDLE find_effect_by_id(EDIT_SECTION* edit, OBJECT_HANDLE object, int64_t id) {
	int num = edit->get_effect_list(object, nullptr, 0);
	if (num <= 0) return nullptr;
	std::vector<EFFECT_HANDLE> effects(num);
	num = edit->get_effect_list(object, effects.data(), num);
	for (int i = 0; i < num; i++) {
		if (edit->get_effect_id(effects[i]) == id) return effects[i];
	}
	return nullptr;
}

// エフェクトの項目を読み取る (移動の有無はget_effect_track_infoのmodeで判定する)
TrackValue read_effect_item(EDIT_SECTION* edit, EFFECT_HANDLE fx, LPCWSTR item) {
	TrackValue tv;
	parse_track_value(edit->get_effect_item_value(fx, item), tv);
	TRACK_INFO info = {};
	if (edit->get_effect_track_info(fx, item, &info, sizeof(info))) tv.moving = (info.mode != nullptr);
	return tv;
}

// エフェクトの項目が使える状態か (通常=単一の数値、キー指定用=キーの数が区間数+1の移動設定)
bool usable_effect_item(const TrackValue& tv, bool keyed, int key_count) {
	return keyed ? keyable(tv, key_count) : tv.plain();
}

// 有効なエフェクトkindのうち、変更する項目が全て使える状態のものがあれば再利用し、無ければ追加して、開始時の値を記録する。
// キー指定用で追加した場合は、各項目に「開始時の値をキーの数だけ並べた直線移動」を設定する
void prepare_effect(EDIT_SECTION* edit, TargetObject& t, int kind, bool keyed) {
	const EffectDef& def = EFFECT_DEFS[kind];
	EffectSlot& slot = t.fx[kind][keyed];
	slot.prepared = true;
	EFFECT_HANDLE fx = nullptr;
	int num = edit->get_effect_list(t.object, nullptr, 0);
	if (num > 0) {
		std::vector<EFFECT_HANDLE> effects(num);
		num = edit->get_effect_list(t.object, effects.data(), num);
		for (int i = 0; i < num && !fx; i++) {
			LPCWSTR name = edit->get_effect_name(effects[i]);
			if (!name || wcscmp(name, def.name) != 0 || !edit->get_effect_enable(effects[i])) continue;
			bool usable = true;
			for (int j = 0; j < def.item_num; j++) {
				usable = usable && usable_effect_item(read_effect_item(edit, effects[i], def.items[j]), keyed, t.key_count);
			}
			if (usable) fx = effects[i];
		}
	}
	if (!fx) {
		fx = edit->create_effect(t.object, def.name);
		slot.created = (fx != nullptr);
		if (fx && keyed) {
			for (int j = 0; j < def.item_num; j++) {
				TrackValue tv = read_effect_item(edit, fx, def.items[j]);
				if (!tv.plain()) continue;
				std::string text;
				for (int k = 0; k < t.key_count; k++) text += tv.keys[0] + ",";
				edit->set_effect_item_value(fx, def.items[j], (text + "直線移動,0").c_str());
			}
		}
	}
	if (!fx) {
		log_line(L"[KeychainToTransform] 「" + std::wstring(def.name) + L"」エフェクトを追加できませんでした");
		return;
	}
	slot.id = edit->get_effect_id(fx);
	for (int i = 0; i < def.item_num; i++) {
		slot.track[i] = read_effect_item(edit, fx, def.items[i]);
		slot.valid[i] = usable_effect_item(slot.track[i], keyed, t.key_count);
		slot.applied[i] = slot.track[i].raw;
	}
}

// 現在の入力状態をオブジェクトへ反映する。
// finish=Cancelの場合は操作開始時の値に戻し、追加したエフェクトを削除する。
// finish=Commitの場合、追加したエフェクトが結果的に未変更なら削除する。
// 変化のある項目が無い場合はcall_edit_section自体を呼ばない (不要なUndo登録を避ける)
void sync_values(Finish finish) {
	struct SlotRef { TargetObject* target; int kind; bool keyed; };
	struct Work {
		std::vector<Write> writes;
		std::vector<SlotRef> prepare;	// エフェクトの検索・追加が必要な対象
		std::vector<SlotRef> remove;	// 追加したエフェクトを削除する対象
	};
	Work work;
	bool use_original = (finish == Finish::Cancel);

	for (auto& t : g_state.targets) {
		std::string v[ITEM_NUM];
		if (use_original) {
			for (int i = 0; i < ITEM_NUM; i++) v[i] = t.track[i].raw;
		} else {
			compute_values(t, v);
		}
		for (int i = 0; i < ITEM_NUM; i++) {
			if (t.track[i].valid) add_write(work.writes, t, 0, t.items[i], v[i], t.applied[i]);
		}

		// 大きさの項目 (オブジェクト本体のエフェクト)
		std::string sz[4];
		if (use_original) {
			for (int i = 0; i < 4; i++) sz[i] = t.size[i].track.raw;
		} else {
			compute_size_values(t, sz);
		}
		for (int i = 0; i < 4; i++) {
			SizeItem& size = t.size[i];
			if (size.item.empty() || sz[i] == size.applied) continue;
			work.writes.push_back({ t.object, t.size_effect.c_str(), 0, size.item.c_str(), sz[i] });
			size.applied = sz[i];
		}

		EffectTarget target = use_original ? EffectTarget{} : effect_target(t);
		if (target.kind >= 0 && !t.fx[target.kind][target.keyed].prepared) work.prepare.push_back({ &t, target.kind, target.keyed });
		for (int kind = 0; kind < FX_NUM; kind++) {
			for (int keyed = 0; keyed < 2; keyed++) {
				EffectSlot& slot = t.fx[kind][keyed];
				if (slot.id == 0) continue;
				const EffectDef& def = EFFECT_DEFS[kind];
				std::string e[4];
				if (use_original) {
					for (int i = 0; i < 4; i++) e[i] = slot.track[i].raw;
				} else {
					compute_effect_values(t, kind, keyed != 0, e);
				}
				bool unchanged = true;
				for (int i = 0; i < def.item_num; i++) unchanged = unchanged && (e[i] == slot.track[i].raw);
				if (finish != Finish::None && slot.created && unchanged) {
					work.remove.push_back({ &t, kind, keyed != 0 });
					continue;
				}
				for (int i = 0; i < def.item_num; i++) {
					if (slot.valid[i]) add_write(work.writes, t, slot.id, def.items[i], e[i], slot.applied[i]);
				}
			}
		}
	}
	if (work.writes.empty() && work.prepare.empty() && work.remove.empty()) return;

	edit_handle->call_edit_section_param(&work, [](void* param, EDIT_SECTION* edit) {
		auto& work = *static_cast<Work*>(param);
		for (auto& w : work.writes) {
			if (w.effect_id == 0) {
				edit->set_object_item_value(w.object, w.base_effect, w.item, w.value.c_str());
			} else if (EFFECT_HANDLE fx = find_effect_by_id(edit, w.object, w.effect_id)) {
				edit->set_effect_item_value(fx, w.item, w.value.c_str());
			}
		}
		for (auto& ref : work.remove) {
			EffectSlot& slot = ref.target->fx[ref.kind][ref.keyed];
			if (EFFECT_HANDLE fx = find_effect_by_id(edit, ref.target->object, slot.id)) edit->delete_effect(ref.target->object, fx);
			slot.id = 0;
		}
		// 追加直後の値は次回の反映から書き込む
		for (auto& ref : work.prepare) prepare_effect(edit, *ref.target, ref.kind, ref.keyed);
	});
}

//=======================================================================
//	状態表示 (HUD)
//=======================================================================
std::wstring format_number(double v, const wchar_t* format) {
	wchar_t buf[64];
	swprintf_s(buf, format, v);
	return buf;
}

// 軸マスクの表示文字列 ("XYZ", "YZ"等)
std::wstring axes_letters(int m) {
	std::wstring s;
	if (m & AXIS_X) s += L"X";
	if (m & AXIS_Y) s += L"Y";
	if (m & AXIS_Z) s += L"Z";
	return s;
}

// キー番号の表示名
std::wstring key_name(const TargetObject& t, int k) {
	if (k == 0) return L"始点";
	if (k == t.key_count - 1) return L"終点";
	return L"中間点" + std::to_wstring(k);
}

bool local_supported();
std::wstring format_setting(double v);

std::wstring describe_state() {
	// 複数オブジェクトを対象にしている場合、キー・エフェクト・カメラの表示は先頭のオブジェクトに基づく
	const TargetObject& first = g_state.targets.front();
	int m = g_state.axes;
	bool single = (m == AXIS_X || m == AXIS_Y || m == AXIS_Z);
	std::wstring axis_label = (m == 0) ? L"" : L"  [" + axes_letters(m) + (single ? L"軸]" : L"平面]");
	if (local_active()) axis_label += L"  [ローカル]";
	if (camera_rotating(first) && g_state.use_effect) axis_label += L"  [目標点中心]";
	if (camera_active(first)) axis_label += L"  [カメラ基準]";
	if (first.base_name != STANDARD_DRAW) axis_label += L"  [" + first.base_name + L"]";
	if (g_state.key_selected) axis_label += L"  [キー: " + key_name(first, selected_key(first)) + L"]";
	NumericInput n = get_numeric_input();
	std::wstring input = n.active ? L"  入力: " + g_state.numeric + L"_" : L"";
	// 数値入力中の変化内容 ("×2.000" / "=150.00" / "+50.00"。式が不完全な間は"?")
	std::wstring numeric_change = !n.valid ? L"?"
		: n.op == NumericOp::Multiply ? L"×" + format_number(n.value, L"%.3f")
		: n.op == NumericOp::Set ? L"=" + format_number(n.value, L"%.2f")
		: format_number(n.value, L"%+.2f");

	std::wstring text;
	switch (g_state.mode) {
		case Mode::Move:
		case Mode::Center: {
			const wchar_t* prefix = (g_state.mode == Mode::Center) ? first.center_label : L"";
			text = std::wstring(g_state.mode == Mode::Center ? L"中心移動" : L"移動") + axis_label + L"\n";
			if (n.active) {
				text += std::wstring(prefix) + axes_letters(numeric_axes()) + L" " + numeric_change;
				break;
			}
			if (camera_active(first)) {
				text += L"右 " + format_number(g_state.mouse_dx * g_settings.move_per_px, L"%+.2f") + L" 下 " + format_number(g_state.mouse_dy * g_settings.move_per_px, L"%+.2f") +
					L" 奥 " + format_number(g_state.wheel * g_settings.move_per_wheel, L"%+.2f");
				break;
			}
			const wchar_t* letters[] = { L"X", L"Y", L"Z" };
			for (int a = 0; a < 3; a++) {
				if (m != 0 && !axis_on(a)) continue;
				text += L"Δ" + std::wstring(prefix) + letters[a] + L" " + format_number(mouse_delta(a, false), L"%+.2f") + L"  ";
			}
			break;
		}
		case Mode::Rotate:
			text = L"回転" + axis_label + L"\n";
			if (n.active) {
				text += axes_letters(numeric_axes()) + L"軸回転 " + numeric_change;
				break;
			}
			for (int a = 0; a < 3; a++) {
				// 軸指定無しのマウス操作はZ軸回転のみ (カメラ制御は3軸)
				if (m == 0 ? (a != 2 && !camera_rotating(first)) : !axis_on(a)) continue;
				const wchar_t* name = first.items[ITEM_RX + a] ? first.items[ITEM_RX + a] : ITEM_NAMES[ITEM_RX + a];
				if (camera_active(first) && a == 2) name = L"視線まわり";
				text += std::wstring(name) + L" " + format_number(mouse_delta(a, camera_rotating(first)), L"%+.2f") + L"°  ";
			}
			break;
		case Mode::Scale:
			// 拡大の数値入力は「2」も「*2」も倍率として扱う
			text = L"拡大" + axis_label + L"\n" + (!n.active
				? L"×" + format_number(mouse_scale_factor(), L"%.3f")
				: !n.valid ? L"?"
				: n.op == NumericOp::Set ? L"=" + format_number(n.value, L"%.3f")
				: L"×" + format_number(n.value, L"%.3f"));
			break;
	}
	text += input;

	// 3行目: 標準描画以外に書き込む場合の説明
	EffectTarget e = effect_target(first);
	if (e.kind >= 0) {
		std::wstring reason;
		if (g_state.use_effect) reason = L"(Alt指定)";
		else if (first.force_effect) reason = L"(" + first.base_name + L"は対応する項目を持たないため)";
		else if (g_state.mode == Mode::Scale && m != 0) {
			reason = L"(軸別の拡大率は標準描画に無いため)";
			int object_axes = scale_object_axes(first);
			if (!local_active() && object_axes != m) reason += L"  画面の" + axes_letters(m) + L"方向の拡大を、オブジェクトの" + axes_letters(object_axes) + L"軸に分けて適用";
		}
		else if (g_state.mode == Mode::Center && first.base->center_by_effect) reason = L"(" + first.base_name + L"は中心を持たないため)";
		else if (e.keyed) reason = L"(移動が未設定の" + (g_state.mode == Mode::Scale ? std::wstring(L"拡大率") : axes_letters(e.mask & AXIS_ALL)) + L"は、移動を設定して追加)";
		else reason = L"(時間変化する値のため自動で追加)";
		text += L"\n→ 「" + std::wstring(EFFECT_DEFS[e.kind].name) + L"」エフェクトを変更 " + reason;
	}
	int sized = size_axes(first);
	if (sized) {
		std::wstring names;
		for (int i = 0; i < 4; i++) {
			int bit = (i == 3) ? ITEM_BIT_ZOOM : (1 << i);
			if (!(sized & bit)) continue;
			if (!names.empty()) names += L"・";
			names += first.size[i].item;
		}
		text += L"\n→ 「" + first.size_effect + L"」の" + names + L"を変更";
	}

	// 操作説明 (意味ごとに1行。今の操作で使えるものだけを表示する)
	bool camera = camera_rotating(first);
	std::vector<std::wstring> help;
	help.push_back(L"確定: Enter・左クリック    キャンセル: Esc・右クリック");
	help.push_back(L"マウス: 値を変更 (押している間の倍率  Shift: ×" + format_setting(g_settings.fine_ratio) + L"  Ctrl: ×" + format_setting(g_settings.coarse_ratio) +
		L"  Shift+Ctrl: ×" + format_setting(g_settings.both_ratio) + L")");
	if (g_state.mode == Mode::Move || g_state.mode == Mode::Center) help.push_back(L"ホイール: 奥行き(Z)を変更");
	if (camera) help.push_back(L"ホイール: Z軸回転(傾き)を変更");
	help.push_back(g_state.mode == Mode::Scale
		? L"数値入力: 2 で2倍、=150 で上書き"
		: L"数値入力: 10 で加算、=10 で上書き、*2・/2 で掛け算・割り算");
	if (n.active) help.push_back(L"Backspace: 入力中の数値を1文字削除");
	std::wstring axis_help = L"X/Y/Z: その軸だけ変更";
	if (local_supported()) {
		// 起点キーが英数字なら、実際のキー名で「G→G」のように示す
		UINT k = g_state.trigger_key;
		bool printable = (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9');
		axis_help += printable ? L" (X→X や " + std::wstring(1, (wchar_t)k) + L"→" + std::wstring(1, (wchar_t)k) + L" でローカル軸)" : L" (X→X でローカル軸)";
	}
	help.push_back(axis_help);
	help.push_back(L"Shift+X/Y/Z: その軸以外の2軸を変更");
	help.push_back(L"←/→: 始点・中間点・終点を選んで、その値を変更 (押すたびに前後へ)");
	help.push_back(L"↓: 始点・中間点・終点の選択を解除");
	if (camera) help.push_back(L"Alt: カメラの位置ではなく目標点を中心に回す");
	else if (!(g_state.mode == Mode::Center && first.base->center_by_effect) && !(g_state.mode == Mode::Scale && first.base->camera)) help.push_back(L"Alt: 「" + std::wstring(EFFECT_DEFS[mode_effect()].name) + L"」エフェクトを追加して、そちらを変更");
	text += L"\n";
	for (auto& line : help) text += L"\n" + line;
	return text;
}

void update_hud() {
	if (!g_hud) return;
	std::wstring text = describe_state();
	SetWindowTextW(g_hud, text.c_str());

	HDC hdc = GetDC(g_hud);
	HGDIOBJ old = SelectObject(hdc, g_hud_font);
	RECT rc = { 0, 0, 0, 0 };
	DrawTextW(hdc, text.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX);
	SelectObject(hdc, old);
	ReleaseDC(g_hud, hdc);

	POINT p;
	GetCursorPos(&p);
	SetWindowPos(g_hud, HWND_TOPMOST, p.x + 20, p.y + 20, rc.right + 16, rc.bottom + 10, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(g_hud, nullptr, TRUE);
}

//=======================================================================
//	入力フック (操作中のみ有効)
//=======================================================================
bool is_modifier_key(WPARAM vk) {
	switch (vk) {
		case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
		case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
		case VK_MENU: case VK_LMENU: case VK_RMENU:
		case VK_LWIN: case VK_RWIN:
			return true;
	}
	return false;
}

// 起点のコマンドを呼び出したキーを推定する。ショートカットキーの割り当ては取得できないため、
// コマンド実行時(キー押下の処理中)に押されている修飾キー以外のキーとする。メニューから実行した場合等は0
UINT detect_trigger_key() {
	for (UINT vk = 0x08; vk <= 0xFE; vk++) { // 0x01～0x06はマウスボタン
		if (is_modifier_key(vk)) continue;
		if (GetAsyncKeyState(vk) & 0x8000) return vk;
	}
	return 0;
}

// ローカル軸に対応した操作か (カメラ制御は移動と回転のみ)
bool local_supported() {
	if (g_state.mode == Mode::Move || g_state.mode == Mode::Rotate) return true;
	return !g_state.targets.front().base->camera;
}

// 軸指定を切り替える。Shift押下中はその軸を除いた2軸(平面)を指定する (Blenderと同じ)。
// 同じ指定を繰り返すと、ローカル軸に対応した操作では「グローバル軸→ローカル軸(起点キーを2回押したのと同じ)→解除」の順に、
// それ以外の操作では「指定→解除」の順に切り替わる (G→X→XでローカルX軸に沿って動くBlenderの操作に合わせる)
void toggle_axis(int axis) {
	int m = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? (AXIS_ALL & ~axis) : axis;
	if (g_state.axes != m) {
		g_state.axes = m;
	} else if (local_supported() && !g_state.local) {
		g_state.local = true;
	} else {
		g_state.axes = 0;
		if (local_supported()) g_state.local = false;
	}
}

// 押されたキーが入力する文字を得る (*等の記号の位置はキーボード配列で異なるため)
wchar_t key_to_char(WPARAM vk, LPARAM lparam) {
	BYTE state[256] = {};
	if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
	wchar_t buf[4] = {};
	// フラグ0x4: キーボードの内部状態(デッドキー等)を変更しない
	int len = ToUnicode((UINT)vk, (UINT)((lparam >> 16) & 0xFF), state, buf, 4, 0x4);
	return (len == 1) ? buf[0] : 0;
}

// 数値入力に1文字追加する。末尾が「-」の時の「-」は、その「-」を消す (符号の切り替え。末尾の「-」は計算結果の符号を反転する)
void append_numeric_char(wchar_t c) {
	if (c == L'=') {
		// 「=」は先頭でのみ有効 (上書き指定)
		if (g_state.numeric.empty()) g_state.numeric += c;
	} else if ((c >= L'0' && c <= L'9') || wcschr(L".+*/", c)) {
		g_state.numeric += c;
	} else if (c == L'-') {
		if (!g_state.numeric.empty() && g_state.numeric.back() == L'-') g_state.numeric.pop_back();
		else g_state.numeric += c;
	}
}

// 変更するキーを前後に移す。未指定の場合は←で現在フレームの直前、→で直後のキーを指定する。
// 範囲は先頭の対象オブジェクトのキーの数で制限する
void step_key(int direction) {
	if (!g_state.key_selected) {
		g_state.key_selected = true;
		g_state.key_offset = (direction > 0) ? 1 : 0;
	} else {
		g_state.key_offset += direction;
	}
	const TargetObject& first = g_state.targets.front();
	g_state.key_offset = std::clamp(g_state.key_offset, -first.prev_key, first.key_count - 1 - first.prev_key);
}

// キーボードフック。キー押下は全て横取りして本体のショートカットが動かないようにする。
// キーアップと修飾キーは本体へそのまま渡す (キー状態の不整合を避けるため)
LRESULT CALLBACK keyboard_hook(int code, WPARAM wparam, LPARAM lparam) {
	if (code != HC_ACTION || !g_state.active) return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
	bool key_up = (lparam & 0x80000000) != 0;
	bool repeat = (lparam & 0x40000000) != 0;	// キーリピート (押しっぱなしでの連続切り替えを防ぐ)

	// Altはキーアップも横取りする (本体のメニューバーが選択状態になるのを防ぐため)
	if (wparam == VK_MENU || wparam == VK_LMENU || wparam == VK_RMENU) {
		if (!key_up && !repeat) g_state.use_effect = !g_state.use_effect;
		return 1;
	}
	if (key_up || is_modifier_key(wparam)) return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

	// 起点キーをもう一度押すとローカル軸での移動に切り替える (Blenderの G→G 相当)
	if (g_state.trigger_key != 0 && wparam == g_state.trigger_key) {
		if (!repeat) g_state.local = !g_state.local;
		return 1;
	}

	switch (wparam) {
		case 'X': toggle_axis(AXIS_X); break;
		case 'Y': toggle_axis(AXIS_Y); break;
		case 'Z': toggle_axis(AXIS_Z); break;
		case VK_LEFT: step_key(-1); break;
		case VK_RIGHT: step_key(1); break;
		case VK_DOWN: g_state.key_selected = false; break;
		case VK_BACK:
			if (!g_state.numeric.empty()) g_state.numeric.pop_back();
			break;
		case VK_RETURN:
			PostMessage(g_hud, WM_APP_FINISH, 1, 0);
			break;
		case VK_ESCAPE:
			PostMessage(g_hud, WM_APP_FINISH, 0, 0);
			break;
		default:
			append_numeric_char(key_to_char(wparam, lparam));
			break;
	}
	return 1;
}

// マウスフック。ボタン押下で終了を予約し、ボタンを離した時点で終了する
// (押下だけ横取りしてボタンアップが本体に届くのを避けるため、アップまで横取りする)
LRESULT CALLBACK mouse_hook(int code, WPARAM wparam, LPARAM lparam) {
	if (code != HC_ACTION || !g_state.active) return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
	switch (wparam) {
		case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
			g_state.pending_finish = 1;
			return 1;
		case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_NCRBUTTONDOWN: case WM_NCRBUTTONDBLCLK:
			g_state.pending_finish = 0;
			return 1;
		case WM_LBUTTONUP: case WM_NCLBUTTONUP: case WM_RBUTTONUP: case WM_NCRBUTTONUP:
			if (g_state.pending_finish >= 0) PostMessage(g_hud, WM_APP_FINISH, g_state.pending_finish, 0);
			return 1;
		case WM_MOUSEWHEEL: {
			// ホイール量はMOUSEHOOKSTRUCTEX::mouseDataの上位ワード (1ノッチ=WHEEL_DELTA)
			short delta = (short)HIWORD(reinterpret_cast<MOUSEHOOKSTRUCTEX*>(lparam)->mouseData);
			double ratio = modifier_ratio();
			if (g_settings.invert_wheel) ratio = -ratio;
			g_state.wheel += (double)delta / WHEEL_DELTA * ratio;
			return 1;
		}
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_NCMBUTTONDOWN: case WM_NCMBUTTONUP: case WM_NCMBUTTONDBLCLK:
		case WM_MOUSEHWHEEL:
			return 1;
	}
	return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

//=======================================================================
//	カメラ制御
//=======================================================================
const wchar_t* CAMERA_EFFECT = L"カメラ制御";

// 指定レイヤーで指定フレームを含むオブジェクトを探す (find_objectは指定フレーム以降を検索するため順に辿る)
OBJECT_HANDLE find_object_at(EDIT_SECTION* edit, int layer, int frame) {
	int from = 0;
	while (OBJECT_HANDLE obj = edit->find_object(layer, from)) {
		OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
		if (lf.start > frame) return nullptr;
		if (lf.end >= frame) return obj;
		if (lf.end + 1 <= from) return nullptr;
		from = lf.end + 1;
	}
	return nullptr;
}

// オブジェクトの基本のエフェクトを決める。BASE_EFFECTSのうち最初に持っているもの、
// いずれも無ければエフェクト一覧の一番上のエフェクト(項目名はGENERIC_ITEM_CANDIDATESから探す)
bool resolve_base_effect(EDIT_SECTION* edit, OBJECT_HANDLE obj, TargetObject& t) {
	for (auto& def : BASE_EFFECTS) {
		if (edit->count_object_effect(obj, def.name) == 0) continue;
		t.base = &def;
		t.base_name = def.name;
		for (int i = 0; i < ITEM_NUM; i++) t.items[i] = def.items[i];
		t.center_label = def.center_label;
		return true;
	}
	int num = edit->get_effect_list(obj, nullptr, 0);
	if (num <= 0) return false;
	std::vector<EFFECT_HANDLE> effects(num);
	num = edit->get_effect_list(obj, effects.data(), num);
	LPCWSTR name = (num > 0) ? edit->get_effect_name(effects[0]) : nullptr;
	if (!name) return false;
	t.base = &GENERIC_BASE;
	t.base_name = name;
	// 「拡大率」「回転」「中心座標」のX/Y/Zは位置ではなく、それぞれ拡大率・回転・中心の値なので位置として扱わない
	bool xyz_not_position = false;
	for (auto effect : { FX_CENTER, FX_ROTATE, FX_SCALE }) {
		if (wcscmp(name, EFFECT_DEFS[effect].name) == 0) xyz_not_position = true;
	}
	for (int i = 0; i < ITEM_NUM; i++) {
		t.items[i] = nullptr;
		if (xyz_not_position && i <= ITEM_Z) continue;
		for (auto candidate : GENERIC_ITEM_CANDIDATES[i]) {
			if (candidate && edit->get_effect_item_value(effects[0], candidate)) {
				t.items[i] = candidate;
				break;
			}
		}
	}
	t.center_label = (t.items[ITEM_CX] && wcsncmp(t.items[ITEM_CX], L"目標", 2) == 0) ? L"目標" : L"中心";
	return true;
}

// トラックバー項目の指定フレームでの値 (取得できなければ設定値の先頭の値、それも無ければdefault_value)
double read_track(EDIT_SECTION* edit, OBJECT_HANDLE obj, LPCWSTR effect, LPCWSTR item, int frame, double default_value) {
	double v;
	if (edit->get_object_track_value(obj, effect, item, frame, &v)) return v;
	TrackValue tv;
	if (parse_track_value(edit->get_object_item_value(obj, effect, item), tv)) return tv.key(0);
	return default_value;
}

bool normalize(double v[3]) {
	double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len < 1e-9) return false;
	for (int i = 0; i < 3; i++) v[i] /= len;
	return true;
}

void cross(const double a[3], const double b[3], double out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

// 対象オブジェクトに影響するカメラ制御(上位レイヤーで最も近く、対象レイヤー数の範囲内のもの)を探し、
// カメラから見た画面の右・下・奥方向の単位ベクトルを求める。
// ※プレビューの編集用視点(右ドラッグで変更した視点)はプラグインから取得できないため反映されない
void find_camera(EDIT_SECTION* edit, TargetObject& t, int layer, int frame) {
	if (!edit->get_object_flag(t.object, OBJECT_FLAG_TYPE::ENABLE_CAMERA)) return;
	for (int l = layer - 1; l >= 0; l--) {
		OBJECT_HANDLE cam = find_object_at(edit, l, frame);
		if (!cam) continue;
		EFFECT_HANDLE camera_effect = edit->find_effect(cam, CAMERA_EFFECT);
		if (!camera_effect || !edit->get_effect_enable(camera_effect)) continue;
		int range = (int)read_track(edit, cam, CAMERA_EFFECT, L"対象レイヤー数", frame, 0);
		if (range > 0 && layer - l > range) continue;

		double pos[3], target[3];
		const wchar_t* pos_items[] = { L"X", L"Y", L"Z" };
		const wchar_t* target_items[] = { L"目標X", L"目標Y", L"目標Z" };
		for (int i = 0; i < 3; i++) {
			pos[i] = read_track(edit, cam, CAMERA_EFFECT, pos_items[i], frame, 0);
			target[i] = read_track(edit, cam, CAMERA_EFFECT, target_items[i], frame, 0);
		}
		// 目標レイヤーが指定されている場合は、そのレイヤーのオブジェクトの座標を目標点とする (実機で要確認)
		int target_layer = (int)read_track(edit, cam, CAMERA_EFFECT, L"目標レイヤー", frame, 0);
		if (target_layer > 0) {
			OBJECT_HANDLE obj = find_object_at(edit, target_layer - 1, frame);
			TargetObject base;
			if (obj && resolve_base_effect(edit, obj, base)) {
				for (int i = 0; i < 3; i++) {
					if (base.items[i]) target[i] = read_track(edit, obj, base.base_name.c_str(), base.items[i], frame, 0);
				}
			}
		}
		double tilt = read_track(edit, cam, CAMERA_EFFECT, L"傾き", frame, 0);

		double forward[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
		if (!normalize(forward)) { forward[0] = 0; forward[1] = 0; forward[2] = 1; }
		camera_basis(forward, tilt, t.cam_right, t.cam_down);
		for (int i = 0; i < 3; i++) t.cam_forward[i] = forward[i];
		t.has_camera = true;
		return;
	}
}

//=======================================================================
//	操作の開始・更新・終了
//=======================================================================
void end_modal(bool commit) {
	if (!g_state.active) return;
	sync_values(commit ? Finish::Commit : Finish::Cancel);
	g_state.active = false;
	if (g_keyboard_hook) { UnhookWindowsHookEx(g_keyboard_hook); g_keyboard_hook = nullptr; }
	if (g_mouse_hook) { UnhookWindowsHookEx(g_mouse_hook); g_mouse_hook = nullptr; }
	KillTimer(g_hud, TIMER_ID_POLL);
	ShowWindow(g_hud, SW_HIDE);
	g_state.targets.clear();
}

// カーソルが画面端に達したら反対側へ回り込ませ、ドラッグ量が途切れないようにする
void wrap_cursor(POINT& p) {
	int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1, bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;
	POINT q = p;
	if (p.x <= left) q.x = right - 1; else if (p.x >= right) q.x = left + 1;
	if (p.y <= top) q.y = bottom - 1; else if (p.y >= bottom) q.y = top + 1;
	if (q.x != p.x || q.y != p.y) {
		SetCursorPos(q.x, q.y);
		p = q;
	}
}

void poll_modal() {
	if (!g_state.active) return;

	// 別アプリへ切り替えられた場合はキー入力を受け取れなくなるためキャンセルする
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	if (pid != GetCurrentProcessId()) {
		end_modal(false);
		return;
	}

	POINT p;
	GetCursorPos(&p);
	double ratio = modifier_ratio();
	g_state.mouse_dx += (p.x - g_state.last_cursor.x) * ratio;
	g_state.mouse_dy += (p.y - g_state.last_cursor.y) * ratio;
	wrap_cursor(p);
	g_state.last_cursor = p;

	sync_values(Finish::None);
	update_hud();
}

// 対象オブジェクトの開始時の状態を読み取る
bool load_target(EDIT_SECTION* edit, OBJECT_HANDLE obj, TargetObject& t) {
	t.object = obj;
	if (!resolve_base_effect(edit, obj, t)) return false;
	int frame = edit->info->frame;
	const wchar_t* base_name = t.base_name.c_str();
	for (int i = 0; i < ITEM_NUM; i++) {
		const wchar_t* item = t.items[i];
		TrackValue& tv = t.track[i];
		if (!item || !parse_track_value(edit->get_object_item_value(obj, base_name, item), tv)) continue;
		TRACK_INFO info = {};
		if (edit->get_object_track_info(obj, base_name, item, &info, sizeof(info))) tv.moving = (info.mode != nullptr);
		t.applied[i] = tv.raw;
		t.now[i] = read_track(edit, obj, base_name, item, frame, tv.key(0));
	}

	// 拡大率の代わりに優先する大きさの項目を、オブジェクト本体(エフェクト一覧の先頭)から探す
	// (後ろに追加されたフィルタ効果の同名の項目を誤って使わないよう、先頭のエフェクトのみを対象にする)
	int effect_num = edit->get_effect_list(obj, nullptr, 0);
	if (effect_num > 0) {
		std::vector<EFFECT_HANDLE> effects(effect_num);
		effect_num = edit->get_effect_list(obj, effects.data(), effect_num);
		LPCWSTR name = (effect_num > 0) ? edit->get_effect_name(effects[0]) : nullptr;
		if (name) {
			t.size_effect = name;
			for (int i = 0; i < 4; i++) {
				for (auto& item : split_item_names(g_settings.size_items[i])) {
					if (!t.size[i].item.empty()) break;
					TrackValue tv = read_effect_item(edit, effects[0], item.c_str());
					if (!tv.valid) continue;
					t.size[i].item = item;
					t.size[i].track = tv;
					t.size[i].applied = tv.raw;
				}
			}
		}
	}

	// キー(始点・中間点・終点)のフレームから、現在フレームの直前・直後のキーを求める
	OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(obj);
	int sections = std::max(1, edit->get_object_section_num(obj));
	t.key_count = sections + 1;
	std::vector<int> key_frames(t.key_count);
	for (int s = 0; s < sections; s++) {
		int f = edit->get_object_section_frame(obj, s);
		key_frames[s] = (f >= 0) ? f : lf.start;
	}
	key_frames[sections] = lf.end;
	t.prev_key = 0;
	for (int s = 0; s < sections; s++) {
		if (key_frames[s] <= frame) t.prev_key = s;
	}

	find_camera(edit, t, lf.layer, frame);
	return true;
}

// 押されたキーの操作で変更できる項目を持っているか
//   特例: 中心を持たないグループ制御の中心移動は中心座標エフェクトで、カメラ制御のX/Y軸回転は目標点で行う
bool supports_mode(const TargetObject& t, Mode mode) {
	auto any_item = [&](int first, int count) {
		for (int i = first; i < first + count; i++) if (t.track[i].valid) return true;
		return false;
	};
	switch (mode) {
		case Mode::Move: return any_item(ITEM_X, 3);
		case Mode::Center: return any_item(ITEM_CX, 3) || t.base->center_by_effect;
		case Mode::Rotate: return any_item(ITEM_RX, 3) || t.base->camera;
		case Mode::Scale: {
			if (t.track[ITEM_ZOOM].valid) return true;
			for (auto& size : t.size) if (!size.item.empty()) return true;
			return false;
		}
	}
	return false;
}

void begin_modal(EDIT_SECTION* edit, Mode mode) {
	if (g_state.active) return;

	std::vector<OBJECT_HANDLE> objects;
	int num = edit->get_selected_object_num();
	for (int i = 0; i < num; i++) {
		OBJECT_HANDLE obj = edit->get_selected_object(i);
		if (obj) objects.push_back(obj);
	}
	if (objects.empty()) {
		OBJECT_HANDLE focus = edit->get_focus_object();
		if (focus) objects.push_back(focus);
	}

	std::vector<TargetObject> targets;
	int skipped = 0;
	for (auto obj : objects) {
		TargetObject t;
		if (!load_target(edit, obj, t)) {
			skipped++;
			continue;
		}
		// 操作に対応する項目を持たない場合は、Alt指定時と同じエフェクトの追加を試し、追加できた場合のみ対象にする
		if (!supports_mode(t, mode)) {
			int kind = mode_effect_of(mode);
			prepare_effect(edit, t, kind, false);
			if (t.fx[kind][0].id == 0) {
				skipped++;
				continue;
			}
			t.force_effect = true;
		}
		targets.push_back(t);
	}
	if (skipped > 0) {
		const wchar_t* mode_names[] = { L"移動", L"中心移動", L"回転", L"拡大" };
		log_line(L"[KeychainToTransform] " + std::wstring(mode_names[(int)mode]) + L"で変更できる項目を持たず、エフェクトも追加できないオブジェクト" + std::to_wstring(skipped) + L"個を対象外にしました");
	}
	if (targets.empty()) return;

	g_state = ModalState{};
	g_state.active = true;
	g_state.mode = mode;
	g_state.targets = targets;
	g_state.trigger_key = detect_trigger_key();
	GetCursorPos(&g_state.last_cursor);

	// フックはメインスレッド(このコールバックの呼び出しスレッド)限定で張る
	DWORD thread_id = GetCurrentThreadId();
	g_keyboard_hook = SetWindowsHookEx(WH_KEYBOARD, keyboard_hook, nullptr, thread_id);
	g_mouse_hook = SetWindowsHookEx(WH_MOUSE, mouse_hook, nullptr, thread_id);
	SetTimer(g_hud, TIMER_ID_POLL, g_settings.poll_ms, nullptr);
	update_hud();
}

void on_move_command(EDIT_SECTION* edit) { begin_modal(edit, Mode::Move); }
void on_center_command(EDIT_SECTION* edit) { begin_modal(edit, Mode::Center); }
void on_rotate_command(EDIT_SECTION* edit) { begin_modal(edit, Mode::Rotate); }
void on_scale_command(EDIT_SECTION* edit) { begin_modal(edit, Mode::Scale); }

//=======================================================================
//	ウィンドウプロシージャ
//=======================================================================
LRESULT CALLBACK hud_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
		case WM_TIMER:
			if (wparam == TIMER_ID_POLL) {
				poll_modal();
				return 0;
			}
			break;

		case WM_APP_FINISH:
			if (g_state.active) {
				poll_modal(); // 最後のマウス移動分を反映してから終了する
				end_modal(wparam == 1);
			}
			return 0;

		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			RECT rc;
			GetClientRect(hwnd, &rc);
			FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
			wchar_t text[2048];
			GetWindowTextW(hwnd, text, 2048);
			HGDIOBJ old = SelectObject(hdc, g_hud_font);
			SetTextColor(hdc, RGB(240, 240, 240));
			SetBkMode(hdc, TRANSPARENT);
			InflateRect(&rc, -8, -5);
			DrawTextW(hdc, text, -1, &rc, DT_NOPREFIX);
			SelectObject(hdc, old);
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;
	}
	return DefWindowProc(hwnd, message, wparam, lparam);
}

//=======================================================================
//	設定ファイル (アプリケーションデータフォルダ\Plugin\KeychainToTransformSettings.ini, 独自key=value形式・UTF-8)
//=======================================================================
std::wstring get_settings_path() {
	return std::wstring(config->app_data_path) + L"\\Plugin\\KeychainToTransformSettings.ini";
}

std::wstring utf8_to_wide(const std::string& s) {
	if (s.empty()) return L"";
	int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), wlen);
	return w;
}
std::string wide_to_utf8(const std::wstring& s) {
	if (s.empty()) return "";
	int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string b(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), b.data(), len, nullptr, nullptr);
	return b;
}

// 数値の設定値を範囲内に収めて反映する (解釈できない値は無視する)
void set_number(const std::wstring& text, double min_value, double max_value, double& out) {
	wchar_t* end = nullptr;
	double v = wcstod(text.c_str(), &end);
	if (end == text.c_str()) return;
	out = std::clamp(v, min_value, max_value);
}

// 設定項目の値を反映する (設定ファイルとダイアログで共通)
void apply_setting(const std::wstring& key, const std::wstring& value) {
	double v;
	if (key == L"move_per_px") set_number(value, 0.001, 1000, g_settings.move_per_px);
	else if (key == L"rotate_per_px") set_number(value, 0.001, 360, g_settings.rotate_per_px);
	else if (key == L"scale_per_px") set_number(value, 0.00001, 1, g_settings.scale_per_px);
	else if (key == L"move_per_wheel") set_number(value, 0.001, 10000, g_settings.move_per_wheel);
	else if (key == L"rotate_per_wheel") set_number(value, 0.001, 360, g_settings.rotate_per_wheel);
	else if (key == L"fine_ratio") set_number(value, 0.001, 1000, g_settings.fine_ratio);
	else if (key == L"coarse_ratio") set_number(value, 0.001, 1000, g_settings.coarse_ratio);
	else if (key == L"both_ratio") set_number(value, 0.001, 1000, g_settings.both_ratio);
	else if (key == L"poll_ms") { v = g_settings.poll_ms; set_number(value, 10, 1000, v); g_settings.poll_ms = (int)v; }
	else if (key == L"invert_camera_pitch") g_settings.invert_camera_pitch = (value == L"1");
	else if (key == L"invert_wheel") g_settings.invert_wheel = (value == L"1");
	else {
		for (int i = 0; i < 4; i++) if (key == SIZE_ITEM_KEYS[i]) g_settings.size_items[i] = value;
	}
}

std::wstring format_setting(double v) {
	wchar_t buf[64];
	swprintf_s(buf, L"%g", v);
	return buf;
}

void load_settings() {
	HANDLE file = CreateFileW(get_settings_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD size = GetFileSize(file, nullptr), read = 0;
	std::string buf(size, '\0');
	ReadFile(file, buf.data(), size, &read, nullptr);
	CloseHandle(file);
	buf.resize(read);
	std::wistringstream iss(utf8_to_wide(buf));
	std::wstring line;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == L'\r') line.pop_back();
		size_t eq = line.find(L'=');
		if (eq != std::wstring::npos) apply_setting(line.substr(0, eq), line.substr(eq + 1));
	}
}

void save_settings() {
	std::wstringstream ss;
	ss << L"move_per_px=" << format_setting(g_settings.move_per_px) << L"\n";
	ss << L"rotate_per_px=" << format_setting(g_settings.rotate_per_px) << L"\n";
	ss << L"scale_per_px=" << format_setting(g_settings.scale_per_px) << L"\n";
	ss << L"move_per_wheel=" << format_setting(g_settings.move_per_wheel) << L"\n";
	ss << L"rotate_per_wheel=" << format_setting(g_settings.rotate_per_wheel) << L"\n";
	ss << L"fine_ratio=" << format_setting(g_settings.fine_ratio) << L"\n";
	ss << L"coarse_ratio=" << format_setting(g_settings.coarse_ratio) << L"\n";
	ss << L"both_ratio=" << format_setting(g_settings.both_ratio) << L"\n";
	ss << L"poll_ms=" << g_settings.poll_ms << L"\n";
	ss << L"invert_camera_pitch=" << (g_settings.invert_camera_pitch ? L"1" : L"0") << L"\n";
	ss << L"invert_wheel=" << (g_settings.invert_wheel ? L"1" : L"0") << L"\n";
	for (int i = 0; i < 4; i++) ss << SIZE_ITEM_KEYS[i] << L"=" << g_settings.size_items[i] << L"\n";
	std::string buf = wide_to_utf8(ss.str());
	HANDLE file = CreateFileW(get_settings_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	WriteFile(file, buf.data(), (DWORD)buf.size(), &written, nullptr);
	CloseHandle(file);
}

//=======================================================================
//	設定ダイアログ (「設定」→「Keychain to Transform設定」から開く)
//	PaletteHistoryと同じく、.rcを使わずCreateWindowExで組んだポップアップ上で独自のメッセージループを回す
//=======================================================================
#define IDC_BTN_OK 2001
#define IDC_BTN_CANCEL 2002

// 数値の設定項目 (ダイアログの行)
struct NumberField {
	const wchar_t* key;
	const wchar_t* label;
	HWND edit;
};
NumberField g_number_fields[] = {
	{ L"move_per_px", L"移動の感度 (1pxあたりの移動量)", nullptr },
	{ L"rotate_per_px", L"回転の感度 (1pxあたりの角度)", nullptr },
	{ L"scale_per_px", L"拡大の感度 (1pxあたりの倍率変化)", nullptr },
	{ L"move_per_wheel", L"ホイール1ノッチあたりの移動量", nullptr },
	{ L"rotate_per_wheel", L"ホイール1ノッチあたりの回転角度", nullptr },
	{ L"fine_ratio", L"Shift押下中の倍率", nullptr },
	{ L"coarse_ratio", L"Ctrl押下中の倍率", nullptr },
	{ L"both_ratio", L"Shift+Ctrl押下中の倍率", nullptr },
	{ L"poll_ms", L"値を反映する間隔 (ミリ秒)", nullptr },
};
// 拡大率の代わりに使う大きさの項目名の入力欄 (g_settings.size_items / SIZE_ITEM_KEYSと同じ順)
struct TextField {
	const wchar_t* label;
	HWND edit;
};
TextField g_size_fields[4] = {
	{ L"オブジェクトのX方向 (拡大率X)", nullptr },
	{ L"オブジェクトのY方向 (拡大率Y)", nullptr },
	{ L"オブジェクトのZ方向 (拡大率Z)", nullptr },
	{ L"全体 (拡大率)", nullptr },
};
HWND g_check_invert_camera_pitch = nullptr;
HWND g_check_invert_wheel = nullptr;
HWND g_static_note = nullptr;

std::wstring setting_text(const wchar_t* key) {
	std::wstring k = key;
	if (k == L"move_per_px") return format_setting(g_settings.move_per_px);
	if (k == L"rotate_per_px") return format_setting(g_settings.rotate_per_px);
	if (k == L"scale_per_px") return format_setting(g_settings.scale_per_px);
	if (k == L"move_per_wheel") return format_setting(g_settings.move_per_wheel);
	if (k == L"rotate_per_wheel") return format_setting(g_settings.rotate_per_wheel);
	if (k == L"fine_ratio") return format_setting(g_settings.fine_ratio);
	if (k == L"coarse_ratio") return format_setting(g_settings.coarse_ratio);
	if (k == L"both_ratio") return format_setting(g_settings.both_ratio);
	if (k == L"poll_ms") return std::to_wstring(g_settings.poll_ms);
	return L"";
}

void apply_settings_from_ui() {
	wchar_t buf[64];
	for (auto& f : g_number_fields) {
		GetWindowTextW(f.edit, buf, 64);
		apply_setting(f.key, buf);
	}
	g_settings.invert_camera_pitch = (SendMessage(g_check_invert_camera_pitch, BM_GETCHECK, 0, 0) == BST_CHECKED);
	g_settings.invert_wheel = (SendMessage(g_check_invert_wheel, BM_GETCHECK, 0, 0) == BST_CHECKED);
	wchar_t text[256];
	for (int i = 0; i < 4; i++) {
		GetWindowTextW(g_size_fields[i].edit, text, 256);
		g_settings.size_items[i] = text;
	}
}

LRESULT CALLBACK config_dlg_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
		case WM_COMMAND:
			switch (LOWORD(wparam)) {
				case IDC_BTN_OK:
					apply_settings_from_ui();
					save_settings();
					log_line(L"[KeychainToTransform] 設定を保存しました");
					DestroyWindow(hwnd);
					return 0;
				case IDC_BTN_CANCEL:
					DestroyWindow(hwnd);
					return 0;
			}
			break;

		case WM_CLOSE:
			DestroyWindow(hwnd);
			return 0;

		case WM_CTLCOLORSTATIC:
			if ((HWND)lparam == g_static_note) {
				HDC hdc = (HDC)wparam;
				SetTextColor(hdc, RGB(110, 110, 110));
				SetBkMode(hdc, TRANSPARENT);
				return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
			}
			break;

		case WM_DESTROY:
			for (auto& f : g_number_fields) f.edit = nullptr;
			for (auto& f : g_size_fields) f.edit = nullptr;
			g_check_invert_camera_pitch = g_check_invert_wheel = g_static_note = nullptr;
			PostQuitMessage(0); // このウィンドウ専用のメッセージループのみを抜ける
			return 0;
	}
	return DefWindowProc(hwnd, message, wparam, lparam);
}

void show_config_dialog(HWND parent, HINSTANCE dll_hinst) {
	const int row_h = 34, label_w = 300, edit_w = 90, text_w = 180, margin = 16;
	const int field_num = (int)(sizeof(g_number_fields) / sizeof(g_number_fields[0]));
	int dw = margin * 2 + label_w + text_w + 20;
	int dh = margin + row_h * field_num + 30 * 2 + 12 + 36 + row_h * 4 + 12 + 70 + 50 + 50;
	int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
	if (parent && IsWindow(parent)) {
		RECT prc;
		GetWindowRect(parent, &prc);
		x = prc.left + ((prc.right - prc.left) - dw) / 2;
		y = prc.top + ((prc.bottom - prc.top) - dh) / 2;
	}

	// ホストから渡されたdll_hinstではなくGetModuleHandle(0)を使う (RegisterClassExと一致させないとウィンドウを作れないため。PaletteHistoryのDESIGN.md参照)
	(void)dll_hinst;
	HINSTANCE hinst = GetModuleHandle(0);
	HWND dlg = CreateWindowEx(WS_EX_DLGMODALFRAME, CONFIG_DLG_CLASS_NAME, L"Keychain to Transform設定",
		WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, dw, dh, parent, nullptr, hinst, nullptr);
	if (!dlg) return;

	int top = margin;
	for (auto& f : g_number_fields) {
		CreateWindowEx(0, WC_STATIC, f.label, WS_VISIBLE | WS_CHILD, margin, top + 4, label_w, 24, dlg, nullptr, hinst, nullptr);
		f.edit = CreateWindowEx(WS_EX_CLIENTEDGE, WC_EDIT, setting_text(f.key).c_str(), WS_VISIBLE | WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
			margin + label_w, top, edit_w, 26, dlg, nullptr, hinst, nullptr);
		top += row_h;
	}
	g_check_invert_camera_pitch = CreateWindowEx(0, WC_BUTTON, L"カメラ制御の上下の向きで、マウスの縦方向を反転する", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
		margin, top, label_w + edit_w, 24, dlg, nullptr, hinst, nullptr);
	top += 30;
	g_check_invert_wheel = CreateWindowEx(0, WC_BUTTON, L"ホイールの向きを反転する", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
		margin, top, label_w + edit_w, 24, dlg, nullptr, hinst, nullptr);
	top += 36;
	// 区切り線
	CreateWindowEx(0, WC_STATIC, L"", WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ, margin, top, label_w + text_w, 2, dlg, nullptr, hinst, nullptr);
	top += 12;
	CreateWindowEx(0, WC_STATIC, L"拡大率の代わりに優先して変更する項目 (カンマ区切り、左を優先)", WS_VISIBLE | WS_CHILD,
		margin, top, label_w + text_w, 24, dlg, nullptr, hinst, nullptr);
	top += 30;
	// 表示順は全体→X→Y→Z (配列の並びはg_settings.size_itemsに合わせてX,Y,Z,全体のまま)
	for (int i : { 3, 0, 1, 2 }) {
		CreateWindowEx(0, WC_STATIC, g_size_fields[i].label, WS_VISIBLE | WS_CHILD, margin, top + 4, label_w, 24, dlg, nullptr, hinst, nullptr);
		g_size_fields[i].edit = CreateWindowEx(WS_EX_CLIENTEDGE, WC_EDIT, g_settings.size_items[i].c_str(), WS_VISIBLE | WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
			margin + label_w, top, text_w, 26, dlg, nullptr, hinst, nullptr);
		top += row_h;
	}
	// 区切り線
	CreateWindowEx(0, WC_STATIC, L"", WS_VISIBLE | WS_CHILD | SS_ETCHEDHORZ, margin, top, label_w + text_w, 2, dlg, nullptr, hinst, nullptr);
	top += 12;
	g_static_note = CreateWindowEx(0, WC_STATIC,
		L"※操作を始めるショートカットキーは、左上メニュー「設定」→「ショート\nカットキーの設定」で「Keychain to Transform: 移動」等に割り当ててください。\n※値を反映する間隔を長くすると、Undoに積まれる件数が減ります。",
		WS_VISIBLE | WS_CHILD, margin, top, label_w + text_w, 60, dlg, nullptr, hinst, nullptr);
	top += 70;
	CreateWindowEx(0, WC_BUTTON, L"OK", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON | WS_GROUP,
		dw / 2 - 110, top, 100, 30, dlg, (HMENU)IDC_BTN_OK, hinst, nullptr);
	CreateWindowEx(0, WC_BUTTON, L"キャンセル", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
		dw / 2 + 10, top, 100, 30, dlg, (HMENU)IDC_BTN_CANCEL, hinst, nullptr);

	// ダイアログ内のコントロールにメッセージフォントを適用する
	for (HWND child = GetWindow(dlg, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		SendMessage(child, WM_SETFONT, (WPARAM)g_hud_font, TRUE);
	}
	SendMessage(g_check_invert_camera_pitch, BM_SETCHECK, g_settings.invert_camera_pitch ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessage(g_check_invert_wheel, BM_SETCHECK, g_settings.invert_wheel ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessage(dlg, DM_SETDEFID, IDC_BTN_OK, 0);

	EnableWindow(parent, FALSE);
	ShowWindow(dlg, SW_SHOW);
	UpdateWindow(dlg);

	// WM_QUITはこのループ自身のGetMessageに消費させる (途中でループを抜けるとホスト側へWM_QUITが流出するため。PaletteHistoryのDESIGN.md参照)
	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && msg.hwnd != nullptr && GetAncestor(msg.hwnd, GA_ROOT) == dlg) {
			DestroyWindow(dlg);
			continue;
		}
		if (!IsDialogMessage(dlg, &msg)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	EnableWindow(parent, TRUE);
	SetForegroundWindow(parent);
}

void on_config_menu(HWND hwnd, HINSTANCE dll_hinst) {
	show_config_dialog(hwnd, dll_hinst);
}

//=======================================================================
//	プラグイン登録関数
//=======================================================================
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	if (g_keyboard_hook) UnhookWindowsHookEx(g_keyboard_hook);
	if (g_mouse_hook) UnhookWindowsHookEx(g_mouse_hook);
	if (g_hud_font) DeleteObject(g_hud_font);
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpszClassName = HUD_CLASS_NAME;
	wcex.lpfnWndProc = hud_proc;
	wcex.hInstance = GetModuleHandle(0);
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	if (!RegisterClassEx(&wcex)) return;

	// 設定ダイアログ用ウィンドウクラス
	WNDCLASSEXW cfg_wcex = {};
	cfg_wcex.cbSize = sizeof(WNDCLASSEX);
	cfg_wcex.lpszClassName = CONFIG_DLG_CLASS_NAME;
	cfg_wcex.lpfnWndProc = config_dlg_proc;
	cfg_wcex.hInstance = GetModuleHandle(0);
	cfg_wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	cfg_wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	if (!RegisterClassEx(&cfg_wcex)) return;

	// マウス操作を透過する半透明の常に手前のポップアップ
	g_hud = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
		HUD_CLASS_NAME, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandle(0), nullptr);
	if (!g_hud) return;
	SetLayeredWindowAttributes(g_hud, 0, 220, LWA_ALPHA);

	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	g_hud_font = CreateFontIndirectW(&ncm.lfMessageFont);

	edit_handle = host->create_edit_handle();
	if (config) load_settings();

	// AviUtl2本体の「設定」メニューに「Keychain to Transform設定」を追加
	if (config) host->register_config_menu(L"Keychain to Transform設定", on_config_menu);
	// AviUtl2本体の「ショートカットキーの設定」でG/R/S等を割り当てる
	host->register_edit_menu(L"Keychain to Transform: 移動", on_move_command);
	host->register_edit_menu(L"Keychain to Transform: 中心移動", on_center_command);
	host->register_edit_menu(L"Keychain to Transform: 回転", on_rotate_command);
	host->register_edit_menu(L"Keychain to Transform: 拡大", on_scale_command);
}
