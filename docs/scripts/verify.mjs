import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { load } from 'cheerio';
import { pathToFileURL } from 'node:url';
import MiniSearch from 'minisearch';
import { docsRoot, repositoryRoot, repository, listFiles, loadCatalog, location } from './catalog.mjs';
import { tokenize } from '../.vitepress/search.mjs';

const destination = path.join(docsRoot, '.vitepress/dist');
const catalog = loadCatalog();
const site = location(process.env.DOCS_SITE_URL);
const pages = new Map(listFiles(destination).filter((file) => file.endsWith('.html')).map((file) => [file, load(fs.readFileSync(file, 'utf8'))]));
const manifest = JSON.parse(fs.readFileSync(path.join(destination, 'build-manifest.json'), 'utf8'));
assert.equal(pages.size, manifest.pages, 'Missing generated pages');
assert.equal(manifest.articles.length, catalog.articles.length);
let checkedLinks = 0;

for (const [file, $] of pages) {
  const relative = path.relative(destination, file).split(path.sep).join('/');
  const pageUrl = new URL(site.base + relative, site.origin);
  for (const element of $('a[href],link[href],script[src],img[src]').toArray()) {
    const href = $(element).attr('href') || $(element).attr('src');
    if (!href || /^(?:mailto:|tel:|data:|javascript:)/i.test(href)) continue;
    const target = new URL(href, pageUrl);
    if (target.origin !== site.origin) continue;
    assert.ok(target.pathname.startsWith(site.base), `${relative}: link escaped publication directory: ${href}`);
    let targetPath = decodeURIComponent(target.pathname.slice(site.base.length));
    if (targetPath.endsWith('/') || !targetPath) targetPath += 'index.html';
    const absolute = path.join(destination, targetPath);
    assert.ok(fs.existsSync(absolute), `${relative}: missing ${href}`);
    if (target.hash && pages.has(absolute)) {
      const id = decodeURIComponent(target.hash.slice(1));
      const found = pages.get(absolute)('[id]').toArray().some((entry) => entry.attribs.id === id);
      assert.ok(found, `${relative}: missing anchor ${href}`);
    }
    checkedLinks++;
  }
}

for (const article of catalog.articles) {
  const $ = pages.get(path.join(destination, article.url, 'index.html'));
  assert.ok($, `Missing article ${article.url}`);
  const heading = $('.vp-doc h1').clone();
  heading.find('.header-anchor').remove();
  assert.equal(heading.text().trim(), article.title);
  assert.ok($(`a[href="${repository}/blob/main/${article.source}"]`).length, `Missing source link ${article.source}`);
  const siblings = catalog.articles.filter((entry) => entry.group === article.group);
  const position = siblings.indexOf(article);
  const neighbors = [siblings[position - 1], siblings[position + 1]].filter(Boolean).map((entry) => site.base + entry.url.slice(1));
  assert.deepEqual($('.VPDocFooter .prev-next a').toArray().map((entry) => entry.attribs.href), neighbors, `Incorrect neighbors: ${article.id}`);
  const source = fs.readFileSync(path.join(repositoryRoot, article.source), 'utf8').replace(/\r\n/g, '\n');
  const code = $('pre code').toArray().map((element) => $(element).text()).join('\n');
  for (const match of source.matchAll(/^```[^\n]*\n([\s\S]*?)^```/gm)) {
    assert.ok(code.includes(match[1].trim()), `Code example changed: ${article.id}`);
  }
}

const indexes = listFiles(path.join(destination, 'assets')).filter((file) => path.basename(file).startsWith('@localSearchIndex') && file.endsWith('.js'));
let records = 0;
const indexedPages = new Set();
for (const file of indexes) {
  const serialized = (await import(pathToFileURL(file).href)).default;
  if (typeof serialized !== 'string') continue;
  const index = MiniSearch.loadJSON(serialized, { fields: ['title', 'titles', 'text'], storeFields: ['title', 'titles'], tokenize });
  records += index.documentCount;
  for (const id of Object.values(JSON.parse(serialized).documentIds)) indexedPages.add(id.split('#')[0]);
  assert.ok(index.search('阅读字体', { combineWith: 'AND' }).some((result) => result.id.includes('/components/font/')), 'Chinese component search failed');
  assert.ok(index.search('crf_create').length, 'API search failed');
}
assert.ok(records > catalog.articles.length, 'Missing search index');
for (const article of catalog.articles) assert.ok(indexedPages.has(site.base + article.url.slice(1)), `Article absent from search: ${article.id}`);
const sitemap = load(fs.readFileSync(path.join(destination, 'sitemap.xml'), 'utf8'), { xmlMode: true });
for (const element of sitemap('loc').toArray()) assert.ok(sitemap(element).text().startsWith(site.url), 'Sitemap URL escaped publication directory');
console.log(`Verified ${pages.size} pages, ${checkedLinks} internal links, ${catalog.articles.length} articles and ${records} search sections.`);
