const { test } = require('node:test');
const assert = require('node:assert/strict');
const { rank } = require('../source/assets/search-worker.js');

const records = [
  { title: '字形缓存', page: '字体服务', group: '公共服务', url: '/services/font/#字形缓存', text: '缓存命中时复用字形；未命中时读取 TTF 文件。' },
  { title: '内存分配', page: '系统架构', group: '平台架构', url: '/architecture/memory/', text: '平台提供字形缓存和应用内存分配，使用 epd_mem_alloc(size)。' },
  { title: '调用示例', page: '组件开发', group: '开发指南', url: '/guides/compute/', text: 'epd_mem_alloc(size) 返回内存地址；失败时返回 NULL。<example>' },
];

test('Chinese phrase matches body and ranks exact heading first', () => {
  const result = rank(records, '字形缓存');
  assert.deepEqual(result.results.map(r => r.url), [records[0].url, records[1].url]);
});
test('API identifiers, fullwidth input and multiple terms are searchable', () => {
  assert.equal(rank(records, 'ＥＰＤ＿ＭＥＭ＿ＡＬＬＯＣ').results.length, 2);
  assert.equal(rank(records, 'epd_mem_alloc NULL').results[0].url, records[2].url);
  assert.equal(rank(records, '字形 TTF').results[0].url, records[0].url);
});
test('Empty queries and unmatched words return no results', () => {
  assert.equal(rank(records, '  ').results.length, 0);
  assert.equal(rank(records, '不存在的术语').results.length, 0);
  assert.equal(rank(records, 'TTF NULL').results.length, 0);
});
test('Search excerpts retain literal characters without producing markup', () => {
  assert.ok(rank(records, '<example>').results[0].excerpt.includes('<example>'));
});
