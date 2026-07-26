// 青空文庫形式 TXT → EPUB3 変換モジュール
// 参考実装: https://github.com/zrn-ns/aozora-epub-api (lib/aozora-parser.ts, chapter-splitter.ts, epub-builder.ts)
// 依存: JSZip (js/jszip.min.js を先にロードすること)
(function () {
  'use strict';

  // ============================================================
  // Aozora Parser
  // ============================================================

  function parseAozoraText(text) {
    var normalized = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    var withoutHeader = removeSymbolBlock(normalized);
    var withoutTitle = removeTitleBlock(withoutHeader);
    return parseLines(withoutTitle);
  }

  // 「テキスト中に現れる記号について」を含む -------ブロックを除去する
  function removeSymbolBlock(text) {
    var lines = text.split('\n');
    var result = [];
    var inBlock = false;
    var blockLines = [];

    for (var i = 0; i < lines.length; i++) {
      var line = lines[i];
      if (!inBlock && /^-{7,}/.test(line)) {
        inBlock = true;
        blockLines = [line];
      } else if (inBlock) {
        blockLines.push(line);
        if (/^-{7,}/.test(line)) {
          var blockContent = blockLines.join('\n');
          if (blockContent.indexOf('【テキスト中に現れる記号について】') === -1) {
            for (var j = 0; j < blockLines.length; j++) result.push(blockLines[j]);
          }
          inBlock = false;
          blockLines = [];
        }
      } else {
        result.push(line);
      }
    }
    if (inBlock) {
      for (var k = 0; k < blockLines.length; k++) result.push(blockLines[k]);
    }
    return result.join('\n');
  }

  // 最初の空行までのタイトル/著者ブロックを本文から取り除く
  function removeTitleBlock(text) {
    var lines = text.split('\n');
    var firstBlankIndex = -1;
    for (var i = 0; i < lines.length; i++) {
      if (lines[i].trim() === '') {
        firstBlankIndex = i;
        break;
      }
    }
    if (firstBlankIndex === -1) return '';
    return lines.slice(firstBlankIndex + 1).join('\n');
  }

  function parseLines(text) {
    var lines = text.split('\n');
    var nodes = [];
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i];
      if (line.trim() === '') {
        nodes.push({ type: 'newline' });
        continue;
      }
      if (line.trim() === '［＃改ページ］') {
        nodes.push({ type: 'pagebreak' });
        continue;
      }
      var indentHeadingMatch = line.match(
        /^［＃([0-9０-９]+)字下げ］(.+)［＃「(.+)」は(大|中|小)見出し］$/
      );
      if (indentHeadingMatch) {
        nodes.push({
          type: 'heading',
          level: headingLevelFromSize(indentHeadingMatch[4]),
          children: parseInline(indentHeadingMatch[3]),
        });
        continue;
      }
      var indentMatch = line.match(/^［＃([0-9０-９]+)字下げ］(.*)$/);
      if (indentMatch) {
        nodes.push({
          type: 'indent',
          chars: parseJapaneseNumber(indentMatch[1]),
          children: parseInline(indentMatch[2]),
        });
        continue;
      }
      var headingMatch = line.match(/^(.*)［＃「(.+)」は(大|中|小)見出し］(.*)$/);
      if (headingMatch) {
        nodes.push({
          type: 'heading',
          level: headingLevelFromSize(headingMatch[3]),
          children: parseInline(headingMatch[2]),
        });
        continue;
      }
      // 通常行: 1行 = 1段落（<br/>羅列を避けCJK縦書きでのメモリ圧を下げる）
      nodes.push({ type: 'paragraph', children: parseInline(line) });
    }
    return nodes;
  }

  function parseJapaneseNumber(s) {
    var normalized = s.replace(/[０-９]/g, function (c) {
      return String.fromCharCode(c.charCodeAt(0) - 0xff10 + 0x30);
    });
    return parseInt(normalized, 10);
  }

  function headingLevelFromSize(size) {
    if (size === '大') return 1;
    if (size === '中') return 2;
    if (size === '小') return 3;
    return 1;
  }

  function parseInline(text) {
    var nodes = [];
    var remaining = text;
    while (remaining.length > 0) {
      var explicitRubyMatch = remaining.match(/^｜([^《]+)《([^》]+)》/);
      if (explicitRubyMatch) {
        nodes.push({ type: 'ruby', base: explicitRubyMatch[1], reading: explicitRubyMatch[2] });
        remaining = remaining.slice(explicitRubyMatch[0].length);
        continue;
      }
      var rubyMatch = remaining.match(/^([一-鿿㐀-䶿々〇ヶ]+)《([^》]+)》/);
      if (rubyMatch) {
        nodes.push({ type: 'ruby', base: rubyMatch[1], reading: rubyMatch[2] });
        remaining = remaining.slice(rubyMatch[0].length);
        continue;
      }
      var emphasisMatch = remaining.match(/^［＃「([^」]+)」に傍点］/);
      if (emphasisMatch) {
        nodes.push({
          type: 'emphasis',
          style: 'sesame',
          children: [{ type: 'text', content: emphasisMatch[1] }],
        });
        remaining = remaining.slice(emphasisMatch[0].length);
        continue;
      }
      var circleEmphasisMatch = remaining.match(/^［＃「([^」]+)」に丸傍点］/);
      if (circleEmphasisMatch) {
        nodes.push({
          type: 'emphasis',
          style: 'circle',
          children: [{ type: 'text', content: circleEmphasisMatch[1] }],
        });
        remaining = remaining.slice(circleEmphasisMatch[0].length);
        continue;
      }
      var aozoraAnnotationMatch = remaining.match(/^［＃[^］]*］/);
      if (aozoraAnnotationMatch) {
        remaining = remaining.slice(aozoraAnnotationMatch[0].length);
        continue;
      }
      var nextSpecialIndex = findNextSpecial(remaining);
      if (nextSpecialIndex > 0) {
        nodes.push({ type: 'text', content: remaining.slice(0, nextSpecialIndex) });
        remaining = remaining.slice(nextSpecialIndex);
      } else if (nextSpecialIndex === 0) {
        nodes.push({ type: 'text', content: remaining[0] });
        remaining = remaining.slice(1);
      } else {
        nodes.push({ type: 'text', content: remaining });
        remaining = '';
      }
    }
    return nodes;
  }

  function findNextSpecial(text) {
    var minIndex = -1;
    var pipeIdx = text.indexOf('｜');
    if (pipeIdx !== -1) minIndex = pipeIdx;
    var rubyMatch = text.match(/[一-鿿㐀-䶿々〇ヶ]+《/);
    if (rubyMatch && rubyMatch.index !== undefined) {
      var idx = rubyMatch.index;
      if (minIndex === -1 || idx < minIndex) minIndex = idx;
    }
    var annoIdx = text.indexOf('［＃');
    if (annoIdx !== -1) {
      if (minIndex === -1 || annoIdx < minIndex) minIndex = annoIdx;
    }
    return minIndex;
  }

  // ============================================================
  // Chapter Splitter
  // ============================================================

  function splitChapters(nodes) {
    var chapters = [];
    var currentTitle = '';
    var currentNodes = [];
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      if (node.type === 'heading') {
        chapters.push({ title: currentTitle, nodes: currentNodes });
        currentTitle = extractHeadingText(node.children);
        currentNodes = [];
      } else if (node.type === 'pagebreak') {
        chapters.push({ title: currentTitle, nodes: currentNodes });
        currentTitle = '';
        currentNodes = [];
      } else {
        currentNodes.push(node);
      }
    }
    chapters.push({ title: currentTitle, nodes: currentNodes });
    return chapters.filter(function (ch) {
      if (ch.title !== '') return true;
      return hasMeaningfulContent(ch.nodes);
    });
  }

  function extractHeadingText(children) {
    var out = '';
    for (var i = 0; i < children.length; i++) {
      var node = children[i];
      if (node.type === 'text') out += node.content;
      else if (node.type === 'ruby') out += node.base;
      else if (node.type === 'emphasis' || node.type === 'indent' || node.type === 'heading') {
        out += extractHeadingText(node.children);
      }
    }
    return out;
  }

  function hasMeaningfulContent(nodes) {
    for (var i = 0; i < nodes.length; i++) {
      var t = nodes[i].type;
      if (t === 'text' || t === 'ruby' || t === 'emphasis' || t === 'indent' || t === 'paragraph' || t === 'pagebreak') {
        return true;
      }
    }
    return false;
  }

  // ============================================================
  // EPUB Builder
  // ============================================================

  function escapeXml(str) {
    return str
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&apos;');
  }

  function nodesToXhtml(nodes) {
    var result = '';
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      switch (node.type) {
        case 'text':
          result += escapeXml(node.content);
          break;
        case 'ruby':
          result += '<ruby>' + escapeXml(node.base) + '<rt>' + escapeXml(node.reading) + '</rt></ruby>';
          break;
        case 'heading':
          // <h> は EPUB リーダーのフォントサイズ設定を上書きするので使わない
          result += '<p><b>' + nodesToXhtml(node.children) + '</b></p>';
          break;
        case 'indent':
          result += '<p style="text-indent: ' + node.chars + 'em">' + nodesToXhtml(node.children) + '</p>';
          break;
        case 'paragraph':
          result += '<p>' + nodesToXhtml(node.children) + '</p>';
          break;
        case 'emphasis':
          result += '<em class="' + node.style + '">' + nodesToXhtml(node.children) + '</em>';
          break;
        case 'pagebreak':
          break;
        case 'newline':
          result += '<br/>\n';
          break;
      }
    }
    return result;
  }

  function buildChapterXhtml(title, nodes) {
    var body = nodesToXhtml(nodes);
    var titleEscaped = escapeXml(title);
    var titleBlock = title ? '<p><b>' + titleEscaped + '</b></p>\n' : '';
    return (
      '<?xml version="1.0" encoding="UTF-8"?>\n' +
      '<!DOCTYPE html>\n' +
      '<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja">\n' +
      '<head>\n' +
      '  <meta charset="UTF-8"/>\n' +
      '  <title>' + titleEscaped + '</title>\n' +
      '  <link rel="stylesheet" type="text/css" href="style.css"/>\n' +
      '</head>\n' +
      '<body>\n' +
      titleBlock +
      body +
      '\n</body>\n</html>'
    );
  }

  function buildContainerXml() {
    return (
      '<?xml version="1.0" encoding="UTF-8"?>\n' +
      '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n' +
      '  <rootfiles>\n' +
      '    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>\n' +
      '  </rootfiles>\n' +
      '</container>'
    );
  }

  function pad3(n) {
    var s = String(n);
    while (s.length < 3) s = '0' + s;
    return s;
  }

  function buildContentOpf(title, author, chapters, uid) {
    var titleEscaped = escapeXml(title);
    var authorEscaped = escapeXml(author);
    var manifestItems = '';
    var spineItems = '';
    for (var i = 0; i < chapters.length; i++) {
      var id = 'chapter_' + pad3(i + 1);
      manifestItems +=
        '    <item id="' + id + '" href="' + id + '.xhtml" media-type="application/xhtml+xml"/>\n';
      spineItems += '    <itemref idref="' + id + '"/>\n';
    }
    return (
      '<?xml version="1.0" encoding="UTF-8"?>\n' +
      '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid" xml:lang="ja">\n' +
      '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n' +
      '    <dc:identifier id="uid">' + escapeXml(uid) + '</dc:identifier>\n' +
      '    <dc:title>' + titleEscaped + '</dc:title>\n' +
      '    <dc:creator>' + authorEscaped + '</dc:creator>\n' +
      '    <dc:language>ja</dc:language>\n' +
      '  </metadata>\n' +
      '  <manifest>\n' +
      '    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>\n' +
      '    <item id="css" href="style.css" media-type="text/css"/>\n' +
      manifestItems +
      '  </manifest>\n' +
      '  <spine page-progression-direction="rtl">\n' +
      '    <itemref idref="nav" linear="no"/>\n' +
      spineItems +
      '  </spine>\n' +
      '</package>'
    );
  }

  function buildNavXhtml(title, chapters) {
    var titleEscaped = escapeXml(title);
    var tocItems = '';
    for (var i = 0; i < chapters.length; i++) {
      var id = 'chapter_' + pad3(i + 1);
      var chapterTitle = chapters[i].title || (title + ' - ' + (i + 1));
      tocItems +=
        '      <li><a href="' + id + '.xhtml">' + escapeXml(chapterTitle) + '</a></li>\n';
    }
    return (
      '<?xml version="1.0" encoding="UTF-8"?>\n' +
      '<!DOCTYPE html>\n' +
      '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja">\n' +
      '<head>\n' +
      '  <meta charset="UTF-8"/>\n' +
      '  <title>' + titleEscaped + '</title>\n' +
      '</head>\n' +
      '<body>\n' +
      '  <nav epub:type="toc">\n' +
      '    <h1>' + titleEscaped + '</h1>\n' +
      '    <ol>\n' +
      tocItems +
      '    </ol>\n' +
      '  </nav>\n' +
      '</body>\n' +
      '</html>'
    );
  }

  function buildStyleCss() {
    return (
      '/* CrossPoint Aozora EPUB Style */\n' +
      'html {\n' +
      '  writing-mode: vertical-rl;\n' +
      '  -epub-writing-mode: vertical-rl;\n' +
      '  -webkit-writing-mode: vertical-rl;\n' +
      '}\n' +
      'ruby rt {\n' +
      '  font-size: 0.5em;\n' +
      '}\n' +
      'em.sesame {\n' +
      '  font-style: normal;\n' +
      '  text-emphasis: sesame;\n' +
      '  -webkit-text-emphasis: sesame;\n' +
      '}\n' +
      'em.circle {\n' +
      '  font-style: normal;\n' +
      '  text-emphasis: circle;\n' +
      '  -webkit-text-emphasis: circle;\n' +
      '}\n' +
      'p[style*="text-indent"] {\n' +
      '  margin: 0;\n' +
      '  padding: 0;\n' +
      '}\n'
    );
  }

  // 疑似UUID (crypto.randomUUID があれば使う、なければ Math.random ベース)
  function makeUid() {
    if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
      return 'urn:uuid:' + crypto.randomUUID();
    }
    var s = '';
    for (var i = 0; i < 32; i++) {
      s += Math.floor(Math.random() * 16).toString(16);
    }
    return 'urn:uuid:' + s.slice(0, 8) + '-' + s.slice(8, 12) + '-' + s.slice(12, 16) + '-' + s.slice(16, 20) + '-' + s.slice(20, 32);
  }

  async function buildEpub(options) {
    if (typeof JSZip === 'undefined') {
      throw new Error('JSZipが読み込まれていません。');
    }
    var title = options.title;
    var author = options.author;
    var chapters = options.chapters;
    var uid = options.uid || makeUid();

    var zip = new JSZip();
    // mimetype は EPUB 仕様により最初のエントリ・非圧縮 (STORE) 必須
    zip.file('mimetype', 'application/epub+zip', { compression: 'STORE' });
    var opts = { compression: 'DEFLATE', compressionOptions: { level: 6 } };
    zip.file('META-INF/container.xml', buildContainerXml(), opts);
    zip.file('OEBPS/content.opf', buildContentOpf(title, author, chapters, uid), opts);
    zip.file('OEBPS/nav.xhtml', buildNavXhtml(title, chapters), opts);
    zip.file('OEBPS/style.css', buildStyleCss(), opts);
    for (var i = 0; i < chapters.length; i++) {
      var filename = 'chapter_' + pad3(i + 1) + '.xhtml';
      zip.file('OEBPS/' + filename, buildChapterXhtml(chapters[i].title, chapters[i].nodes), opts);
    }
    return await zip.generateAsync({
      type: 'blob',
      mimeType: 'application/epub+zip',
    });
  }

  // ============================================================
  // Public helpers
  // ============================================================

  // 冒頭2行からタイトル/著者を抽出、失敗時はファイル名フォールバック
  function extractTitleAuthor(text, filename) {
    var normalized = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    var lines = normalized.split('\n');
    var line1 = (lines[0] || '').trim();
    var line2 = (lines[1] || '').trim();
    var line3 = (lines[2] || '').trim();
    var fallbackTitle = filename.replace(/\.txt$/i, '');
    if (line1 && line2 && line3 === '') {
      return { title: line1, author: line2 };
    }
    if (line1 && line2 === '') {
      return { title: line1, author: '不明' };
    }
    return { title: fallbackTitle, author: '不明' };
  }

  async function convertTxtToEpub(file, progressCallback) {
    var cb = typeof progressCallback === 'function' ? progressCallback : function () {};

    cb(0.05);
    var buf = await file.arrayBuffer();
    var text;
    try {
      text = new TextDecoder('utf-8', { fatal: true }).decode(buf);
    } catch (e) {
      throw new Error('UTF-8デコードに失敗しました。Shift_JISファイルは非対応です。');
    }

    cb(0.15);
    var meta = extractTitleAuthor(text, file.name);

    cb(0.30);
    var nodes = parseAozoraText(text);
    if (nodes.length === 0) {
      throw new Error('本文が空です。');
    }

    cb(0.50);
    var chapters = splitChapters(nodes);
    if (chapters.length === 0) {
      throw new Error('章分割の結果が空になりました。');
    }

    cb(0.70);
    var blob = await buildEpub({
      title: meta.title,
      author: meta.author,
      chapters: chapters,
    });

    cb(1.0);
    var epubName = file.name.replace(/\.txt$/i, '.epub');
    return { blob: blob, name: epubName };
  }

  window.AozoraEpub = {
    parseAozoraText: parseAozoraText,
    splitChapters: splitChapters,
    buildEpub: buildEpub,
    extractTitleAuthor: extractTitleAuthor,
    convertTxtToEpub: convertTxtToEpub,
  };
})();
