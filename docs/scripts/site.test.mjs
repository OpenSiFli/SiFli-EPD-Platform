import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { parse } from 'yaml';
import MiniSearch from 'minisearch';
import { loadCatalog, location, sourceMap, resolveLink, urlFor, slugify, repositoryRoot } from './catalog.mjs';
import { tokenize } from '../.vitepress/search.mjs';

test('publication base matches the COS directory', () => {
  const workflow = parse(fs.readFileSync(path.join(repositoryRoot, '.github/workflows/deploy-docs.yml'), 'utf8'));
  const publisher = workflow.jobs.publish.steps.find((step) => step.uses === 'OpenSiFli/SiFliMirrorSync@v1');
  assert.equal(location(workflow.env.DOCS_SITE_URL).base, publisher.with.prefix);
  for (const invalid of ['http://example.com/', 'https://name:secret@example.com/docs/', 'https://example.com/docs?q=1',
    'https://example.com/docs#part', 'https://example.com/a//b', 'https://example.com/a%2fb']) {
    assert.throws(() => location(invalid));
  }
});

test('all authored documents and component READMEs have unique routes', () => {
  const catalog = loadCatalog();
  assert.equal(catalog.articles.length, 46);
  assert.equal(catalog.articles.filter((entry) => entry.component).length, 5);
  assert.equal(new Set(catalog.articles.map((entry) => entry.url)).size, 46);
  assert.equal(urlFor('user/user-guide.md', 'device'), '/device/user-guide/');
  assert.equal(urlFor('development/first-application.md', 'guides'), '/guides/development/first-application/');
  assert.equal(urlFor('components/font/README.md', 'components'), '/components/font/');
});

test('source-relative links resolve from their original source directories', () => {
  const sources = sourceMap(loadCatalog());
  assert.equal(resolveLink('../architecture/architecture.md#双核', 'docs/content/user/user-guide.md', sources), '/architecture/architecture/#双核');
  assert.equal(resolveLink('include/font_wire.h', 'components/compute_reader_font/README.md', sources),
    'https://github.com/OpenSiFli/SiFli-EPD-Platform/blob/main/components/compute_reader_font/include/font_wire.h');
  assert.equal(resolveLink('../../public/assets/system-architecture.svg', 'docs/content/architecture/architecture.md', sources), '/assets/system-architecture.svg');
  assert.throws(() => resolveLink('./missing.md', 'docs/content/user/user-guide.md', sources));
  assert.equal(slugify('开关机、锁屏和返回'), '开关机锁屏和返回');
});

test('Chinese phrases and C identifiers are searchable in the browser', () => {
  const browserTokenize = new Function(`return (${tokenize.toString()})`)();
  const options = { fields: ['text'], tokenize: browserTokenize };
  const index = new MiniSearch(options);
  index.addAll([{ id: 'font', text: '阅读字体的字形缓存由 ACPU 处理。' }, { id: 'api', text: '应用接口 epd_app_request_redraw' }]);
  const client = MiniSearch.loadJSON(JSON.stringify(index), options);
  assert.equal(client.search('字形缓存', { combineWith: 'AND' })[0].id, 'font');
  assert.equal(client.search('epd_app_request_redraw')[0].id, 'api');
  assert.equal(client.search('不存在的功能', { combineWith: 'AND' }).length, 0);
});
