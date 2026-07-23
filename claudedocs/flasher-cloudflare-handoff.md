# Web Flasher を flasher.xtc.hr20k.com で公開する — ハンドオフ手順書

作成日: 2026-07-21
対象リポジトリ: `aGFydWtp/crosspoint-jp`(ローカル: `/Users/haruki/Documents/crosspoint-jp`)
関連サービス: `xtc.hr20k.com`(Cloudflare Worker `url-to-xtc`、ソース: `/Users/haruki/Documents/html2xtc`)

---

## 1. 背景と目的

- 上流フォーク `zrn-ns/crosspoint-jp` には ESP Web Tools ベースの Web Flasher が実装済みで、`https://zrn-ns.github.io/crosspoint-jp/` で稼働している。
- 本リポジトリ(`aGFydWtp/crosspoint-jp`)はそのフォークであり、Flasher の仕組み(`flasher/index.html` と `.github/workflows/deploy-flasher.yml`)をそのまま引き継いでいる。ワークフローはすべて `${{ github.repository }}` と既定の `GITHUB_TOKEN` で完結しており、リポジトリ名のハードコードはない。
- 目的: 自リポジトリでリリースを作成し、Flasher を **`https://flasher.xtc.hr20k.com/`**(hr20k.com は Cloudflare 管理)で公開する。

### なぜ Cloudflare Workers(assets-only)方式か

- `xtc.hr20k.com` と同じデプロイパターン(`wrangler deploy` + `custom_domain: true`)に揃えられる。
- Workers の Custom Domain は、デプロイ時に対象ホスト名の DNS レコードと **Advanced Certificate をゾーン上に自動発行**する。Universal SSL は `*.hr20k.com` の 1 階層しかカバーしないが、この仕組みにより 2 階層サブドメイン `flasher.xtc.hr20k.com` でも追加費用なしで有効な証明書が付く。
  - 根拠: [Workers Custom Domains — Certificates](https://developers.cloudflare.com/workers/configuration/routing/custom-domains/)("Creating a Custom Domain will also generate an Advanced Certificate on your target zone for your target hostname.")
- GitHub Release のアセットは CORS ヘッダを返さないため、ブラウザから直接 fetch できない。既存設計(上流コミット `e463f76`)は「CI 実行時にバイナリをダウンロードして静的サイトに同梱し、manifest と同一オリジンで配信する」方式でこれを解決しており、Cloudflare でもこの設計をそのまま踏襲する。

---

## 2. 実施済みの変更(ローカル、未コミット)

`feature/xtc-library` ブランチの作業ツリー上にある。**コミットはまだ行っていない。**

| ファイル | 状態 | 内容 |
|---|---|---|
| `flasher/wrangler.jsonc` | 新規 | assets-only Worker `crosspoint-flasher` の設定。`assets.directory: "../_site"`、`workers_dev: false`、route `flasher.xtc.hr20k.com`(`custom_domain: true`) |
| `.github/workflows/deploy-flasher.yml` | 変更 | 最終ステップを `peaceiris/actions-gh-pages@v4`(GitHub Pages)→ `npx wrangler@4 deploy --config flasher/wrangler.jsonc` に置換。`permissions.contents` を `write` → `read` に降格(gh-pages push 廃止のため。`gh release download` は read で足りる) |
| `.gitignore` | 変更 | `_site/` を追加(CI と wrangler dry-run が生成する作業ディレクトリ) |

検証済み: `npx wrangler@4 deploy --config flasher/wrangler.jsonc --dry-run` がローカルで成功(設定のパースとアセット解決を確認。実デプロイは未実施)。

変更しなかったもの:

- `deploy-flasher.yml` のバイナリ収集・manifest 生成ステップ(dev = 最新 `dev-*` prerelease、stable = 最新非 prerelease で `sd-*` 除外、の選択ロジック含む)
- `flasher/index.html`(manifest を相対パスで参照しているため、どのオリジンでもそのまま動く)
- `release.yml` / `dev-build.yml`

---

## 3. 残作業チェックリスト

### 3.1 コミットと push

- [ ] 上記 3 ファイルを専用ブランチ(例: `feature/flasher-cloudflare`)でコミットし、`master` にマージして push
  - 注意: `deploy-flasher.yml` の `workflow_run` トリガーは**デフォルトブランチ上のワークフロー定義**しか発火しないため、`master` に入るまで自動デプロイは動かない

### 3.2 Cloudflare 側(要ダッシュボード操作)

- [ ] API トークン作成: My Profile → API Tokens → テンプレート **「Edit Cloudflare Workers」**
  - スコープ: `url-to-xtc` をデプロイしているアカウント + `hr20k.com` ゾーン
- [ ] Account ID の確認(html2xtc のデプロイで使用しているものと同一)

### 3.3 GitHub 側(aGFydWtp/crosspoint-jp、要 Web UI 操作)

- [ ] Settings → Secrets and variables → Actions に以下を追加:
  - `CLOUDFLARE_API_TOKEN`(3.2 で作成したトークン)
  - `CLOUDFLARE_ACCOUNT_ID`
- [ ] Actions タブで Actions を有効化(フォーク直後は無効になっている)

### 3.4 最初のリリース作成

Flasher は**自リポジトリの GitHub Release** からバイナリを取得する。フォークにはリリースが引き継がれないため、最低 1 つ作る必要がある。

- dev チャンネル: `master` へ push → `Dev Build` ワークフローが `dev-<timestamp>` の prerelease を自動作成
- stable チャンネル: バージョンタグ(例: `v0.1.10`)を push → `Compile Release` ワークフローが Release を作成

いずれかの完了後、`Deploy Flasher` が `workflow_run` で自動起動する(Actions タブから `workflow_dispatch` で手動起動も可)。

### 3.5 動作確認

- [ ] `Deploy Flasher` の実行ログで `wrangler deploy` が成功していること
- [ ] Cloudflare ダッシュボード → Workers & Pages に `crosspoint-flasher` が作成され、Custom Domain `flasher.xtc.hr20k.com` が Active であること(DNS レコードと証明書は自動作成される)
- [ ] `https://flasher.xtc.hr20k.com/` を Chrome または Edge で開き、チャンネル選択と Install ボタンが表示されること
- [ ] `https://flasher.xtc.hr20k.com/manifest_dev.json`(または `manifest_stable.json`)が 200 で返ること
- [ ] 実機(Xteink X4)を USB-C 接続して書き込みが完走すること

---

## 4. トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| `wrangler deploy` が 403 / authentication error | トークンのスコープ不足。「Edit Cloudflare Workers」テンプレートで再作成し、対象アカウント・ゾーンを確認 |
| Custom Domain の作成に失敗する | `hr20k.com` ゾーンに `flasher.xtc` の既存 DNS レコードがあると衝突する。DNS 設定から手動レコードを削除して再デプロイ |
| Deploy Flasher が「No dev release found」でスキップ | 自リポジトリにリリースが 1 つもない。§3.4 を先に実施 |
| Install ボタンが無反応 / シリアルポートが出ない | Web Serial API は Chrome / Edge 89+ のみ対応。Safari / Firefox では動かない(ブラウザ側の制約であり、ホスティングでは解決不可) |
| デバイスが認識されない | デバイス側の書き込みロック解除(電源ボタン操作)を確認。上流 README の Web installer 手順を参照 |

---

## 5. 今後の任意タスク

- [ ] `xtc.hr20k.com`(html2xtc の frontend、Svelte SPA)に Flasher へのリンクを追加(設置場所は未決定: 設定タブ / フッター等)
- [ ] `README.md` 53-64 行目付近の Flasher URL(現在は zrn-ns の GitHub Pages を指している)を `https://flasher.xtc.hr20k.com/` に更新
- [ ] 上流 `zrn-ns/crosspoint-jp` を将来 merge する際、`deploy-flasher.yml` のデプロイステップが GitHub Pages 方式に戻されないよう注意(コンフリクト時は Cloudflare 方式を優先)

---

## 6. 参考

- 稼働中の上流 Flasher: https://zrn-ns.github.io/crosspoint-jp/
- 本家 Flasher(方式が異なる。任意 .bin の手動アップロード型): https://crosspointreader.com/#flash-tools
- ESP Web Tools: https://esphome.github.io/esp-web-tools/
- Workers Custom Domains: https://developers.cloudflare.com/workers/configuration/routing/custom-domains/
- Universal SSL の階層制限: https://developers.cloudflare.com/ssl/edge-certificates/universal-ssl/limitations/
- manifest のパーティションオフセット: bootloader=0x0, partitions=0x8000 (32768), firmware=0x10000 (65536)(`deploy-flasher.yml` 内で生成)
