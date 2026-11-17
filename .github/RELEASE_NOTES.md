
## 更新対象のファイル / Files to update

- dinput8.dll
- ff13-2-run.bat
- ff13-2-practice.bat
- README.md / README.ja.md

## 更新内容

### 追加

- 「ホットキーと設定」の画面に「Modなし起動で使うカテゴリー」を追加(既定: なし)
  - カテゴリーを選んでおくと、`ff13-2-run.bat`でModなしで起動したときに、そのカテゴリーのセーブデータ1〜40をセーブ一覧に表示します

### 変更

- 同梱のbatファイル
  - `dinput8.dll`がFPS FixなどこのModのものでない場合は、ファイル名を変えず、ゲームも起動せずに止まるようにしました
  - `dinput8.dll`と`dinput8.dll.off`が両方あるときは、その旨を表示して止まるようにしました
- README
  - 「他のModとの併用」を追加(FPS Fixとは併用できません。Drop Modとは併用できます)

---

## Changes

### Added

- "Category for starts without the mod" on the "Hotkeys and settings" screen (default: none)
  - With a category picked, `ff13-2-run.bat` (the start without the mod) lists that category's saves 1-40

### Changed

- The included bat files
  - When `dinput8.dll` is not this mod's (the FPS Fix, for example), they stop without renaming anything or starting the game
  - When both `dinput8.dll` and `dinput8.dll.off` are present, they say so and stop
- README
  - Added "Using it with other mods" (not with the FPS Fix; fine with the Drop Mod)
