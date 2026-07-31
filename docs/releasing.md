# リリース手順

## リリースする

GitHub の **Actions → Release (dispatch) → Run workflow** から実行する。ローカルでの `git tag` / `git push --tags` は不要。

| 入力 | 説明 |
|------|------|
| `bump` | `patch` / `minor` / `major`。直近の `v*` タグから次の番号を算出する |
| `prerelease` | RC としてリリースする。`v0.1.13-rc1` のような連番になる |
| `dry_run` | 算出結果をサマリに出すだけで、タグは打たない |

タグを push したあと `release-dispatch.yml` が `release.yml` を起動し、ビルドと GitHub Release の作成まで自動で進む。

実行前に対象コミットが `master` にマージ済みであること。`master` 以外のブランチからの実行はワークフロー側で拒否される。

## バージョンの決まり方

**git tag が唯一の情報源**で、`platformio.ini` にバージョンは書かれていない。`scripts/git_branch.py` が全環境の `CROSSPOINT_VERSION` を組み立てる。

| ビルド | バージョン文字列 | 意味 |
|--------|------------------|------|
| リリース（タグ上） | `0.1.13` | `v0.1.13` そのもの |
| リリース（タグ外） | `0.1.12-15-g173ad08b` | v0.1.12 から 15 コミット進んだ地点のビルド |
| dev (`env:default`) | `0.1.12-dev-my-branch-173ad08b` | ブランチ名と commit 付き |

タグ外のリリースビルドに `-15-g173ad08b` が付くのは意図的で、手元でうっかり作った成果物が正規リリースと見分けられなくなるのを防ぐため。

`build_flags` で `CROSSPOINT_VERSION` を定義した環境はその値を維持する。OTA の検証で特定バージョンを名乗らせたい場合は `platformio.local.ini`（gitignore 済み）で上書きする。

```ini
[env:ota_rc_test]
extends = base
build_flags =
  ${base.build_flags}
  -DCROSSPOINT_VERSION=\"0.1.12-rc1\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=1
custom_i18n_languages = ENGLISH, JAPANESE
```

`-rc` を含むバージョンは `OtaUpdater::isUpdateNewer()` が同一バージョン番号でも「更新あり」と判定するため（`src/network/OtaUpdater.cpp`）、GitHub に新しいリリースを作らずに OTA のダウンロード経路を検証できる。

## 注意点

**CI のチェックアウトには `fetch-depth: 0` が必要。** バージョンは `git describe` で求めるので、浅いクローンではタグに到達できず `0.0.0` になる。ビルドを行うワークフローには設定済み。

**`GITHUB_TOKEN` 由来のイベントはワークフローを起動しない。** ワークフローの無限ループを防ぐための GitHub の仕様で、`workflow_dispatch` と `repository_dispatch` だけが例外。リリースの連鎖はこれに二度引っかかるため、いずれも `gh workflow run` で明示的に起動している。

| 連鎖 | 本来の契機 | 発火しない理由 | 対処 |
|------|------------|----------------|------|
| `release-dispatch.yml` → `release.yml` | タグ push | `GITHUB_TOKEN` で push したタグは push イベントを発生させない | `gh workflow run release.yml --ref <タグ>` |
| `release.yml` → `deploy-flasher.yml` | `workflow_run` | `release.yml` の run 自体が `GITHUB_TOKEN` 由来なので完了時に `workflow_run` が発生しない | `gh workflow run deploy-flasher.yml --ref master` |

そのため `release.yml` には `workflow_dispatch` トリガー、タグ以外の ref を弾く `if: startsWith(github.ref, 'refs/tags/')` ガード、`actions: write` 権限が必要。後者の連鎖は `github.event_name == 'workflow_dispatch'` のときだけ起動する（タグ push 経由なら `workflow_run` が通常どおり連鎖するため）。

**fork 由来のタグを持ち込まない。** `--match 'v[0-9]*'` で絞ったうえで、`git describe` は HEAD の祖先しか見ないため通常は問題にならない。過去に cjk-fork 由来の `v0.2.x` / `v0.3.0` が混入していたが削除済み。

## 関連ワークフロー

| ファイル | 契機 | 用途 |
|----------|------|------|
| `release-dispatch.yml` | 手動 | バージョン算出、タグ作成、`release.yml` の起動 |
| `release.yml` | タグ push または `release-dispatch.yml` からの起動 | ビルドと Release 作成 |
| `release_candidate.yml` | 手動（`release/*` ブランチ） | RC ビルド |
| `dev-build.yml` | master への push | Dev Build のプレリリース |
