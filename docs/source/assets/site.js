(() => {
  const $ = selector => document.querySelector(selector);
  const base = document.documentElement.dataset.base || '';
  const menu = $('.menu-toggle'), navigation = $('#site-navigation'), scrim = $('.nav-scrim');
  function toggleMenu(open) {
    navigation.classList.toggle('is-open', open);
    menu.setAttribute('aria-expanded', String(open));
    menu.setAttribute('aria-label', open ? '关闭导航' : '打开导航');
    scrim.hidden = !open;
  }
  menu.addEventListener('click', () => toggleMenu(menu.getAttribute('aria-expanded') !== 'true'));
  scrim.addEventListener('click', () => toggleMenu(false));
  navigation.addEventListener('click', event => { if (event.target.closest('a')) toggleMenu(false); });
  $('.theme-toggle').setAttribute('aria-label', document.documentElement.dataset.theme === 'dark' ? '切换浅色模式' : '切换深色模式');
  $('.theme-toggle').addEventListener('click', () => {
    const theme = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = theme;
    $('.theme-toggle').setAttribute('aria-label', theme === 'dark' ? '切换浅色模式' : '切换深色模式');
    try { localStorage.setItem('epd-docs-theme', theme); } catch (_) { /* Storage may be disabled. */ }
  });
  document.querySelectorAll('.markdown pre').forEach(code => {
    let block = code.closest('.highlighter-rouge') || code.closest('.highlight');
    if (!block) {
      block = document.createElement('div');
      code.replaceWith(block); block.append(code);
    }
    block.classList.add('highlight');
    const button = document.createElement('button');
    button.className = 'copy-code'; button.textContent = '复制'; button.setAttribute('aria-label', '复制代码');
    button.addEventListener('click', async () => {
      try {
        if (navigator.clipboard && window.isSecureContext) await navigator.clipboard.writeText(code.innerText);
        else {
          const field = document.createElement('textarea'); field.value = code.innerText;
          field.className = 'sr-only'; document.body.append(field); field.select();
          const copied = document.execCommand('copy'); field.remove();
          if (!copied) throw new Error('Copy unavailable');
          button.focus();
        }
        button.textContent = '已复制';
      } catch (_) { button.textContent = '复制失败'; }
      setTimeout(() => { button.textContent = '复制'; }, 1800);
    });
    block.append(button);
  });
  const toc = [...document.querySelectorAll('.page-toc a')];
  const top = $('.back-top');
  let pending = false;
  function updatePosition() {
    pending = false;
    top.hidden = window.scrollY < 500;
    let current = toc[0];
    for (const link of toc) {
      const heading = document.getElementById(decodeURIComponent(link.hash.slice(1)));
      if (heading && heading.getBoundingClientRect().top <= 125) current = link;
    }
    for (const link of toc) {
      link.classList.toggle('active', link === current);
      if (link === current) link.setAttribute('aria-current', 'location');
      else link.removeAttribute('aria-current');
    }
  }
  window.addEventListener('scroll', () => { if (!pending) { pending = true; requestAnimationFrame(updatePosition); } }, { passive: true });
  top.addEventListener('click', () => { window.scrollTo({ top: 0, behavior: 'instant' }); $('#main-content').focus({ preventScroll: true }); });
  updatePosition();

  const dialog = $('.search-dialog'), input = $('#search-input'), results = $('#search-results'), status = $('.search-status');
  const more = $('.search-more');
  let worker, timer, request = 0, matches = [], shown = 0, terms = [], restoreFocus;
  function searchWorker() {
    if (worker) return worker;
    worker = new Worker(base + '/assets/search-worker.js');
    worker.addEventListener('message', event => {
      if (event.data.id !== request) return;
      if (event.data.error) { status.textContent = '搜索暂时不可用，请稍后重试。'; results.replaceChildren(); more.hidden = true; return; }
      matches = event.data.results; terms = event.data.terms; shown = 0; results.replaceChildren();
      status.textContent = matches.length ? '找到 ' + matches.length + ' 个结果' : '没有找到相关文档。';
      renderMore();
    });
    worker.addEventListener('error', () => { status.textContent = '搜索暂时不可用，请稍后重试。'; });
    return worker;
  }
  function highlight(text) {
    const fragment = document.createDocumentFragment();
    const escaped = terms.filter(Boolean).map(term => term.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
    if (!escaped.length) { fragment.append(document.createTextNode(text)); return fragment; }
    const pattern = new RegExp(escaped.join('|'), 'gi');
    let start = 0;
    for (const match of text.matchAll(pattern)) {
      fragment.append(document.createTextNode(text.slice(start, match.index)));
      const mark = document.createElement('mark'); mark.textContent = match[0]; fragment.append(mark);
      start = match.index + match[0].length;
    }
    fragment.append(document.createTextNode(text.slice(start))); return fragment;
  }
  function renderMore() {
    for (const result of matches.slice(shown, shown + 20)) {
      const item = document.createElement('li'), link = document.createElement('a');
      link.href = result.url;
      const title = document.createElement('strong'); title.append(highlight(result.title));
      const context = document.createElement('small'); context.textContent = result.group + ' / ' + result.page;
      const excerpt = document.createElement('p'); excerpt.append(highlight(result.excerpt));
      link.append(title, context, excerpt); item.append(link); results.append(item);
    }
    shown += 20; more.hidden = shown >= matches.length;
  }
  function search() {
    request++;
    const query = input.value.trim();
    if (!query) { results.replaceChildren(); status.textContent = '输入关键词搜索正文和计算组件说明。'; more.hidden = true; return; }
    status.textContent = '正在搜索…';
    searchWorker().postMessage({ id: request, query, url: base + '/search-index.json' });
  }
  input.addEventListener('input', () => {
    request++; clearTimeout(timer); results.replaceChildren(); more.hidden = true;
    timer = setTimeout(search, 120);
  });
  input.addEventListener('keydown', event => {
    if (event.isComposing) return;
    if (event.key === 'ArrowDown') { event.preventDefault(); results.querySelector('a')?.focus(); }
    if (event.key === 'Enter') { event.preventDefault(); results.querySelector('a')?.click(); }
  });
  results.addEventListener('keydown', event => {
    if (!['ArrowDown', 'ArrowUp'].includes(event.key)) return;
    event.preventDefault();
    const links = [...results.querySelectorAll('a')], index = links.indexOf(document.activeElement);
    const next = index + (event.key === 'ArrowDown' ? 1 : -1);
    if (next < 0) input.focus(); else links[Math.min(next, links.length - 1)]?.focus();
  });
  more.addEventListener('click', renderMore);
  function openSearch() {
    toggleMenu(false); restoreFocus = document.activeElement;
    if (!dialog.open) dialog.showModal();
    input.focus(); searchWorker();
  }
  $('.search-trigger').addEventListener('click', openSearch);
  $('.search-close').addEventListener('click', () => dialog.close());
  dialog.addEventListener('close', () => restoreFocus?.focus());
  dialog.addEventListener('click', event => {
    const rect = dialog.getBoundingClientRect();
    if (event.target === dialog && (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom)) dialog.close();
  });
  document.addEventListener('keydown', event => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') { event.preventDefault(); openSearch(); }
    if (event.key === 'Escape' && !event.isComposing) {
      toggleMenu(false);
      if (dialog.open) dialog.close();
    }
  });
})();
