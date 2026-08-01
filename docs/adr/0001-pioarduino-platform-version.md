# ADR-0001: pioarduino プラットフォームのバージョンを本家に合わせ、コピーバグはパッチで回避する

- 日付: 2026-08-02
- ステータス: 採用
- 関連: PR #96, `scripts/patch_espidf_libcopy.py`, `platformio.ini` の `custom_sdkconfig`

## 背景

このフォークは `custom_sdkconfig` で mbedTLS のレコードバッファを
`ASYMMETRIC_CONTENT_LEN` / `IN=16384` / `OUT=2048` に設定している。
ESP32-C3 は PSRAM を持たず、48KB のフレームバッファが `.bss` を占めるため、
mbedTLS 既定の対称 16KB（接続あたり約 33KB）では TLS ハンドシェイクが
ヒープに収まらない。

しかし `custom_sdkconfig` を設定した瞬間から 2026-08-02 まで、**この設定は
一度も実機に届いていなかった**。pioarduino `55.03.37` の
`idf_lib_copy`（`builder/frameworks/espidf.py`）が、ソースビルドした
ESP-IDF のアーカイブをパッケージへ戻す際に `os.listdir()` で 1 階層しか
走査せず、`esp-idf/mbedtls/mbedtls/library/` にある upstream mbedTLS の
アーカイブ（`libmbedtls_2.a` = `ssl_tls.c` / `ssl_msg.c` を含む）を
コピーしないため。リンカは 2 月のプレビルドを使い続けていた。

この 14KB の差が、読書セッション後のフォントダウンロードが
`MBEDTLS_ERR_RSA_PUBLIC_FAILED + MBEDTLS_ERR_MPI_ALLOC_FAILED`（`-0x4290`）
で失敗する原因だった。

このバグは pioarduino `55.03.311`（2026-07-24）で `copy_idf_component_archives()`
として修正済み。`55.03.38` / `38-1` / `39` には入っていない。

## 選択肢

### A. プラットフォームを `55.03.311` に更新する

- 利点: パッチ不要。5 か月分の上流修正も入る
- 欠点:
  - Arduino core 3.3.7 → 3.3.11、ESP-IDF 5.5.2 → 5.5.5 を同伴する。
    SD (SdFat)、E-Ink SPI、WiFi、EPUB 解析、フォントレンダリングが
    すべてフレームワーク依存なので、TLS だけでなく全機能の再検証が必要
  - Flash 使用率が 92.1%（6,278,039 / 6,815,744、余裕約 525KB）。
    IDF と core の更新で数十〜200KB 増えることは普通にあり、
    収まらなければパーティション変更まで波及する
  - **本家 crosspoint-reader (v1.4.1) も `55.03.37` である。**
    バージョンが食い違うと、CLAUDE.md に記載の upstream 追従フロー
    (`git merge upstream/<タグ>`) で毎回 `platformio.ini` が衝突し、
    フレームワークの再インストールも巻き込む

### B. `custom_sdkconfig` をやめて既定のプレビルドに戻る

- 利点: バグを踏まなくなる。本家と完全に同じ構成
- 欠点: レコードバッファが対称 16KB に戻り、TLS ハンドシェイクの
  ヒープ問題が再発する。本家は `custom_sdkconfig` を使わない代わりに
  この問題を抱えたままで（`HTTP_RX_BUF = 4096` 固定、プロファイル分けなし）、
  こちらはフォント・青空文庫のダウンロードを実用にする必要がある

### C. `55.03.37` に留まり、コピー処理をパッチで直す（採用）

- 利点:
  - 本家と同じプラットフォームを維持でき、upstream 追従が素直
  - 検証範囲が TLS 周辺に限定される
  - Flash 使用率が変わらない
- 欠点: 共有パッケージへのパッチを抱える。プラットフォームの
  version bump で外れる可能性がある

## 決定

**C を採用する。** 本家との整合を優先する。

`scripts/patch_espidf_libcopy.py` で `idf_lib_copy` に入れ子アーカイブの
再帰コピーを冪等注入する。既存の `patch_jpegdec.py` / `patch_pngdec.py` と
同じ作法。

このパッチは借金ではなく、**本家がプラットフォームを上げるまでの橋渡し**と
位置づける。そのため以下の性質を持たせた:

- `copy_idf_component_archives` の存在を検出し、**修正済みプラットフォームでは
  何もしない**。将来 version bump してもスクリプトを触る必要がない
- アンカーが見つからない場合は黙って通さず警告を出す。プラットフォームの
  構造が変わったときに、設定が効かない firmware を気付かず出荷しないため
- `rglob` を `mbedtls` 配下に限定する。`lib_src` 全体を走査すると他
  コンポーネントの入れ子アーカイブも拾い、リネーム規則が `libmbedtls.a`
  しか特別扱いしないため basename 衝突時に無警告で上書きしうる

## 将来バージョンを上げるときの判断材料

**上げる契機**: 本家 crosspoint-reader がプラットフォームを上げたとき。
それに追従するのが基本。単独で先に上げる合理性は薄い。

**上げる前に確認すること**:

1. Flash に収まるか。上げた時点の使用率を確認する。`partitions.csv` の
   app パーティションは約 6.8MB で、2026-08-02 時点で 92.1% を使っている
2. `scripts/patch_espidf_libcopy.py` を**削除できるか**。新バージョンに
   `copy_idf_component_archives` があれば不要。スクリプトは自動で無効化
   されるので残しておいても動くが、不要になったら消すこと
3. `custom_sdkconfig` の値が**本当に実機に届いているか**。これは
   `sdkconfig.<env>` を見ても分からない（後述）
4. `custom_component_remove` で除外しているコンポーネント
   (RainMaker / Insights 系ほか 12 個) が、新バージョンでも同じ理由で
   除外必要か
5. `scripts/patch_pngdec.py` / `patch_jpegdec.py` が新バージョンでも
   必要か。どちらもソースビルド固有の問題への対処

**設定が届いたことの確認方法**（今回これを知らずに何度も誤った報告をした）:

`sdkconfig.<env>` とパッケージ側の `sdkconfig` は **要求値** であって
実機の値ではない。コピー処理がパッケージの `sdkconfig` をプロジェクトの
要求値で上書きするため、確認すると常に要求通りに見える。見るべきは:

- `framework-arduinoespressif32-libs/<chip>/sdkconfig.orig`
  — プレビルドが実際に使った設定
- `framework-arduinoespressif32-libs/<chip>/lib/libmbedtls_2.a` の
  **タイムスタンプとサイズ**。component ラッパー (`libmbedtls.a`) だけが
  新しく upstream (`libmbedtls_2.a`) が古ければ、コピーが効いていない
- `ar t libmbedtls_2.a | grep ssl_msg` — レコードバッファのコードが
  どのアーカイブにあるかの確認
- **firmware のハッシュが変わったか**。アーカイブを直しても
  `build_cache_dir` から `firmware.elf` が再利用され、前回と同一
  バイナリになることがある。`PLATFORMIO_BUILD_CACHE_DIR` を一時
  ディレクトリに向けて強制リンクし、ハッシュの変化を確認する

**再ビルドの強制方法**: `sdkconfig.defaults` 先頭の `# TASMOTA__<hash>` が
キャッシュキー。`custom_sdkconfig` を変えれば自動で再ビルドされる。
変えずに強制したい場合はこのファイルを削除する。`pio run -t clean` は
この経路に無関係。

## 影響

- 共有プラットフォーム (`~/.platformio/platforms/espressif32`) を書き換える。
  同一マシンの他プロジェクトの `idf_lib_copy` も変わるが、挙動としては
  修正なので害はない
- pioarduino への issue 報告は不要（既に修正済み）
- 本家へこの TLS 設定を還元する場合、本家が `custom_sdkconfig` を採用すると
  同じバグを踏むため、プラットフォーム更新とセットで提案する必要がある
