let indexPromise;
function normalize(value) { return value.normalize('NFKC').toLowerCase(); }
function rank(records, query) {
  const terms = normalize(query).trim().split(/\s+/).filter(Boolean);
  if (!terms.length) return { results: [], terms };
  const results = [];
  for (const record of records) {
    const title = normalize(record.title), page = normalize(record.page), text = normalize(record.text);
    const combined = title + ' ' + page + ' ' + normalize(record.group) + ' ' + text;
    if (!terms.every(term => combined.includes(term))) continue;
    let score = 0;
    for (const term of terms) score += (title === term ? 100 : title.includes(term) ? 30 : 0) + (page.includes(term) ? 15 : 0) + (text.includes(term) ? 1 : 0);
    const position = Math.max(0, ...terms.map(term => text.indexOf(term)).filter(n => n >= 0).slice(0, 1));
    const start = Math.max(0, position - 45);
    results.push({ title: record.title, page: record.page, group: record.group, url: record.url, score,
      excerpt: (start ? '…' : '') + record.text.slice(start, start + 170) + (record.text.length > start + 170 ? '…' : '') });
  }
  results.sort((a, b) => b.score - a.score || a.title.localeCompare(b.title, 'zh-CN'));
  return { results, terms };
}
if (typeof self !== 'undefined') self.addEventListener('message', async event => {
  const { id, query, url } = event.data;
  try {
    indexPromise ||= fetch(url).then(response => { if (!response.ok) throw new Error(response.status); return response.json(); }).catch(error => { indexPromise = undefined; throw error; });
    self.postMessage({ id, ...rank(await indexPromise, query) });
  } catch (_) { self.postMessage({ id, error: true }); }
});
if (typeof module !== 'undefined') module.exports = { rank };
