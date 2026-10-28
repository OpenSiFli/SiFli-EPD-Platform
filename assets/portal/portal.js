'use strict';
const $ = (s, root = document) => root.querySelector(s);
const state = { path: '/', offset: 0, limit: 40, more: false, listing: 0, settings: 0, info: 0, authenticated: false, saving: false, epoch: 0, csrf: '', username: '' };
const requests = new Set(), uploads = new Set(), delays = new Map();
const downloads = new Map();
const wirelessExpanded = new Set();
const settingDisabled = new WeakMap();
const settingsOrder = ['wireless', 'display', 'fonts', 'lock', 'sound', 'applications', 'ai', 'speech', 'time', 'reset', 'device'];
const settingsNavigation = { active: 'wireless', entries: [] };
const wallpaperPicker = { path: '/', offset: 0, more: false, generation: 0, control: null };
let packageJob = null, packagePanel = null, packagePolling = false;
const readingFonts = { data: null, job: null, timer: null, generation: 0, detail: null, sending: false, uploading: false, request: null, text: '', percent: null };
const serviceConfigs = Object.fromEntries(['ai', 'speech'].map(kind => [kind, { data: null, generation: 0, sending: false, job: null, editor: null }]));
const serviceDisabled = new WeakMap();
let serviceFieldId = 0, serviceApplications = [];
const packageStorageKey = 'portal-package-job';
const auth = { mode: 'checking', busy: false, retryUntil: 0, retryTimer: null, logoutToken: '' };
const icons = { folder: 'folder', 'folder-plus': 'folder-add', upload: 'upload', download: 'download', device: 'device', settings: 'settings', refresh: 'refresh', file: 'file' };
function icon(name) {
  const node = document.createElement('span');
  node.className = 'asset-icon';
  node.setAttribute('aria-hidden', 'true');
  const img = document.createElement('img');
  img.src = '/icons/' + (icons[name] || 'file') + '.svg';
  img.alt = ''; img.width = img.height = 24;
  node.append(img); return node;
}
document.querySelectorAll('[data-icon]').forEach(node => node.replaceWith(icon(node.dataset.icon)));
function el(tag, text, cls) { const node = document.createElement(tag); if (text !== undefined) node.textContent = text; if (cls) node.className = cls; return node; }
function notice(message) { if (!state.authenticated) return; $('#notice span').textContent = message; $('#notice').hidden = false; }
$('#notice button').onclick = () => $('#notice').hidden = true;
const failures = { 400: '请求内容无效', 401: '登录已失效，请重新登录', 403: '此操作不可用，请刷新后重试', 404: '文件或页面不存在', 409: '设备正在处理其他操作', 413: '文件超过支持的大小', 429: '操作过于频繁，请稍后重试', 507: '存储空间不足', 503: '设备暂时不可用' };
const authFailures = {
  busy: '设备正在处理其他操作，请稍后重试。',
  invalid_request: '输入内容未通过设备校验，请检查后重试。',
  auth_unavailable: '设备登录服务尚未就绪，请稍后重试。',
  auth_store_corrupt: '设备账号数据无法读取，请在设备上检查账号设置。',
  storage_error: '设备存储操作失败，请检查设备存储后重试。',
  crypto_error: '设备暂时无法完成安全验证，请稍后重试。',
  account_exists: '该用户名已存在，请检查设备账号设置。',
  account_limit: '设备已达到 5 个账号的上限，请在设备上管理账号。',
  account_not_found: '账号已不存在，请在设备上检查账号设置。',
  setup_closed: '首次设置窗口已关闭，请在设备上重新开启。',
  invalid_credentials: '用户名、密码或验证码不正确，请重新输入。',
  bad_credentials: '用户名、密码或验证码不正确，请重新输入。',
  rate_limited: '尝试次数过多，请稍后重试。',
  unauthenticated: '登录已失效，请重新登录。',
  csrf_invalid: '登录状态已变更，请刷新登录状态后重试。'
};
function cancelled() { return new DOMException('操作已取消', 'AbortError'); }
function current(epoch) { return state.authenticated && epoch === state.epoch; }
function report(error, epoch = state.epoch) { if (current(epoch) && error.name !== 'AbortError') notice(error.message); }
async function api(path, options = {}, publicRequest = false, responseType = 'json') {
  if (!publicRequest && !state.authenticated) throw cancelled();
  const epoch = state.epoch, controller = new AbortController(), headers = new Headers(options.headers);
  if (!publicRequest && !['GET', 'HEAD', 'OPTIONS'].includes((options.method || 'GET').toUpperCase())) headers.set('X-CSRF-Token', state.csrf);
  requests.add(controller);
  const timeout = responseType === 'blob' ? null : setTimeout(() => controller.abort(), 20000);
  try {
    const response = await fetch('/api/' + path, { ...options, headers, cache: 'no-store', credentials: 'same-origin', signal: controller.signal });
    if (epoch !== state.epoch) throw cancelled();
    if (response.status === 401 && !publicRequest) { endSession('登录已失效，请重新登录。'); throw cancelled(); }
    let detail = null;
    try { if (response.status !== 204) detail = response.ok && responseType === 'blob' ? await response.blob() : await response.json(); } catch (error) { if (error.name === 'AbortError') throw error; }
    if (epoch !== state.epoch) throw cancelled();
    if (!response.ok) {
      const code = typeof detail?.error === 'string' ? detail.error : '';
      const error = new Error(authFailures[code] || detail?.message || failures[response.status] || '请求失败');
      error.code = code;
      error.status = response.status;
      const retry = response.headers.get('Retry-After');
      error.retryAfter = /^\d+$/.test(retry || '') ? Number(retry) : retry ? Math.ceil((Date.parse(retry) - Date.now()) / 1000) : Number(detail?.retry_after);
      if (!publicRequest && code === 'csrf_invalid') { endSession('登录状态已变更，请重新登录。'); throw cancelled(); }
      throw error;
    }
    if (response.status !== 204 && !detail) throw new Error('设备返回了无效响应，请重试。');
    return detail;
  } catch (error) {
    if (epoch !== state.epoch) throw cancelled();
    if (error.name === 'AbortError') throw new Error('连接设备超时，请检查网络后重试。');
    if (error instanceof TypeError) throw new Error('无法连接设备，请检查热点或 Wi-Fi 连接。');
    throw error;
  } finally { clearTimeout(timeout); requests.delete(controller); }
}
function pause(ms, epoch) {
  if (!current(epoch)) return Promise.reject(cancelled());
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { delays.delete(timer); current(epoch) ? resolve() : reject(cancelled()); }, ms);
    delays.set(timer, reject);
  });
}
function clearSession() {
  ++state.epoch; ++state.listing; ++state.settings; ++state.info;
  state.authenticated = false; state.csrf = ''; state.username = ''; state.saving = false;
  state.path = '/'; state.offset = 0; state.more = false;
  wirelessExpanded.clear(); ++wallpaperPicker.generation; wallpaperPicker.control = null;
  $('#wallpaper-list').replaceChildren(); $('#wallpaper-path').textContent = ''; $('#wallpaper-status').textContent = '';
  packageJob = packagePanel = null; packagePolling = false;
  clearTimeout(readingFonts.timer); ++readingFonts.generation;
  Object.assign(readingFonts, { data: null, job: null, timer: null, detail: null, sending: false, uploading: false, request: null, text: '', percent: null });
  $('#font-upload').reset(); $('#font-list').replaceChildren(); $('#font-detail').replaceChildren(); $('#font-detail').hidden = true;
  $('#font-current').textContent = '正在读取阅读字体…'; $('#font-status').textContent = '';
  $('#font-progress').hidden = $('#font-cancel').hidden = true;
  clearServiceConfigs();
  settingsNavigation.entries.forEach(entry => entry.panel.hidden = true);
  settingsNavigation.entries = []; settingsNavigation.active = 'wireless';
  $('#settings-tabs').replaceChildren();
  requests.forEach(controller => controller.abort()); requests.clear();
  uploads.forEach(request => request.abort()); uploads.clear();
  delays.forEach((reject, timer) => { clearTimeout(timer); reject(cancelled()); }); delays.clear();
  downloads.forEach((timer, url) => { clearTimeout(timer); URL.revokeObjectURL(url); }); downloads.clear();
  clearInterval(auth.retryTimer); auth.retryTimer = null; auth.retryUntil = 0; auth.busy = false;
  $('.workspace').hidden = true; $('.workspace').inert = true; $('#sign-in').hidden = false;
  document.querySelectorAll('dialog[open]').forEach(dialog => { dialog.returnValue = 'cancel'; dialog.close(); });
  for (const id of ['file-list', 'transfers', 'breadcrumbs', 'settings-groups', 'device-details', 'edit-fields']) $('#' + id).replaceChildren();
  syncSettingsBusy(); $('#file-list').removeAttribute('aria-busy');
  for (const id of ['session-username', 'page-number', 'confirm-title', 'confirm-detail', 'edit-title']) $('#' + id).textContent = '';
  $('#device-name').textContent = '设备'; $('#info-name').textContent = '墨水屏';
  for (const id of ['storage-used', 'storage-free']) $('#' + id).textContent = '—';
  $('.storage-track>span').style.width = '0%'; $('.storage-track').removeAttribute('aria-valuenow');
  $('#file-empty').hidden = $('.pagination').hidden = true;
  $('#upload').disabled = $('#new-folder').disabled = true;
  $('#file-input').value = ''; $('#folder-form').reset(); $('#folder-name').setCustomValidity('');
  $('#folder-form button[type=submit]').disabled = false;
  $('#login').reset(); resetSecrets();
  $('#notice').hidden = true; $('#notice span').textContent = '';
  document.title = '登录 · 墨水屏';
}
function endSession(message) { clearSession(); showAuth('login', message, true); }
function json(method, value) { return { method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(value) }; }
function bytes(value) {
  if (!Number.isFinite(value)) return '—';
  const units = ['B', 'KB', 'MB', 'GB', 'TB']; let i = 0;
  while (value >= 1024 && i < units.length - 1) { value /= 1024; ++i; }
  return (i ? value.toFixed(value < 10 ? 1 : 0) : value) + ' ' + units[i];
}
function child(name) { return (state.path === '/' ? '/' : state.path + '/') + name; }
function datetime(value) {
  if (!value) return '—';
  const date = new Date(value * 1000);
  return Number.isNaN(date.getTime()) ? '—' : date.toLocaleDateString('zh-CN');
}
function crumbs() {
  const root = $('#breadcrumbs'); root.replaceChildren();
  const parts = ['TF 卡', ...state.path.split('/').filter(Boolean)];
  parts.forEach((name, i) => {
    if (i) root.append(el('span', '/', 'slash'));
    const item = el('button', name); item.onclick = () => openPath('/' + parts.slice(1, i + 1).join('/'));
    root.append(item);
  });
}
function capacity(storage) {
  $('#storage-used').textContent = storage?.mounted ? '已用 ' + bytes(storage.used) : '未插入 TF 卡';
  $('#storage-free').textContent = storage?.mounted ? '可用 ' + bytes(Math.max(0, storage.total - storage.used)) : '';
  const ratio = storage?.total ? Math.min(100, 100 * storage.used / storage.total) : 0;
  $('.storage-track>span').style.width = ratio + '%'; $('.storage-track').setAttribute('aria-valuenow', ratio.toFixed(0));
  $('#upload').disabled = $('#new-folder').disabled = !storage?.mounted || storage?.busy;
}
async function loadFiles() {
  if (!state.authenticated) return;
  const generation = ++state.listing; crumbs();
  $('#file-list').setAttribute('aria-busy', 'true');
  try {
    const result = await api('files?' + new URLSearchParams({ path: state.path, offset: state.offset, limit: state.limit }));
    if (generation !== state.listing) return;
    capacity(result.storage); state.more = result.more;
    const fragment = document.createDocumentFragment();
    result.entries.forEach(file => {
      const row = el('div', undefined, 'file-row'); row.setAttribute('role', 'row');
      const name = el(file.directory ? 'button' : 'span', undefined, 'file-name');
      name.append(icon(file.directory ? 'folder' : 'file'), el('span', file.name));
      if (file.directory) name.onclick = () => openPath(child(file.name));
      row.append(name, el('span', datetime(file.modified), 'file-date'), el('span', file.directory ? '—' : bytes(file.size), 'file-size'));
      const actions = el('div', undefined, 'file-actions');
      const path = child(file.name);
      if (!file.directory) {
        const download = el('a', undefined, 'file-download'); download.href = '/api/file?' + new URLSearchParams({ path: child(file.name) });
        download.setAttribute('aria-label', '下载 ' + file.name); download.download = file.name; download.append(icon('download')); actions.append(download);
        download.onclick = event => { event.preventDefault(); downloadFile(download); };
        if (wallpaperImage(file.name)) actions.append(actionButton('设为墙纸', () => updateSetting({ id: 'wallpaper', label: '墙纸文件' }, path)));
      }
      if (file.writable) {
        const remove = actionButton('删除', () => deleteFile(file, path, remove));
        remove.setAttribute('aria-label', '删除 ' + file.name); remove.disabled = !!result.storage?.busy; actions.append(remove);
      }
      row.append(actions);
      fragment.append(row);
    });
    $('#file-list').replaceChildren(fragment); $('#file-empty').hidden = result.entries.length > 0;
    $('#file-empty p').textContent = result.storage?.mounted ? '文件夹为空' : '未插入 TF 卡';
    $('#previous').disabled = state.offset === 0; $('#next').disabled = !state.more;
    $('#page-number').textContent = String(state.offset / state.limit + 1);
    $('.pagination').hidden = state.offset === 0 && !state.more;
  } catch (error) { if (generation === state.listing) report(error); }
  finally { if (generation === state.listing) $('#file-list').removeAttribute('aria-busy'); }
}
function openPath(path) { if (!state.authenticated) return; state.path = path; state.offset = 0; loadFiles(); }
async function deleteFile(file, path, button) {
  if (!state.authenticated || button.disabled) return;
  const epoch = state.epoch, directory = state.path, offset = state.offset;
  if (!await confirmAction(file.directory ? '删除文件夹？' : '删除文件？', file.name + (file.directory ? '。只能删除空文件夹。' : '。删除后无法恢复。'))) return;
  button.disabled = true;
  try {
    await api('file?' + new URLSearchParams({ path }), { method: 'DELETE' });
    if (!current(epoch)) return;
    if (state.path === directory && state.offset === offset && $('#file-list').children.length === 1 && offset > 0) state.offset = Math.max(0, offset - state.limit);
    await loadFiles(); notice('已删除 ' + file.name);
  } catch (error) { report(error, epoch); }
  finally { if (current(epoch)) button.disabled = false; }
}
function wallpaperImage(name) { return /\.(png|jpe?g)$/i.test(name); }
function closeWallpaperPicker() { ++wallpaperPicker.generation; $('#wallpaper-dialog').close(); }
async function loadWallpaperFiles() {
  const epoch = state.epoch, generation = ++wallpaperPicker.generation;
  if (!state.authenticated || !$('#wallpaper-dialog').open) return;
  const { path, offset } = wallpaperPicker, list = $('#wallpaper-list');
  $('#wallpaper-path').textContent = path === '/' ? 'TF 卡' : 'TF 卡' + path;
  $('#wallpaper-up').disabled = path === '/';
  $('#wallpaper-previous').disabled = $('#wallpaper-next').disabled = true;
  list.replaceChildren(); list.setAttribute('aria-busy', 'true'); $('#wallpaper-status').textContent = '正在读取图片…';
  try {
    const result = await api('files?' + new URLSearchParams({ path, offset, limit: state.limit }));
    if (!current(epoch) || generation !== wallpaperPicker.generation || !$('#wallpaper-dialog').open) return;
    wallpaperPicker.more = !!result.more;
    for (const file of result.entries) {
      if (!file.directory && !wallpaperImage(file.name)) continue;
      const target = (path === '/' ? '/' : path + '/') + file.name;
      const button = actionButton('', () => {
        if (file.directory) { wallpaperPicker.path = target; wallpaperPicker.offset = 0; loadWallpaperFiles(); }
        else { const control = wallpaperPicker.control; closeWallpaperPicker(); updateSetting(control, target); }
      });
      button.className = 'wallpaper-file'; button.append(icon(file.directory ? 'folder' : 'file'), el('span', file.name), el('span', file.directory ? '打开' : '使用', 'connection-label'));
      button.disabled = !file.directory && !!result.storage?.busy; list.append(button);
    }
    $('#wallpaper-status').textContent = !result.storage?.mounted ? '未插入 TF 卡。' : list.children.length ? '点击图片即可设为墙纸。' : '本页没有 PNG/JPEG 图片。' + (result.more ? '可继续查看下一页。' : '可先在“文件”中上传图片。');
    $('#wallpaper-previous').disabled = offset === 0; $('#wallpaper-next').disabled = !result.more;
  } catch (error) { if (current(epoch) && generation === wallpaperPicker.generation) $('#wallpaper-status').textContent = error.message; }
  finally { if (generation === wallpaperPicker.generation) list.removeAttribute('aria-busy'); }
}
function chooseWallpaper(control) {
  if (!state.authenticated || state.saving) return;
  wallpaperPicker.control = control; wallpaperPicker.path = '/'; wallpaperPicker.offset = 0;
  $('#wallpaper-dialog').showModal(); loadWallpaperFiles();
}
$('#wallpaper-close').onclick = closeWallpaperPicker;
$('#wallpaper-dialog').addEventListener('cancel', () => { ++wallpaperPicker.generation; });
$('#wallpaper-up').onclick = () => { wallpaperPicker.path = wallpaperPicker.path.slice(0, wallpaperPicker.path.lastIndexOf('/')) || '/'; wallpaperPicker.offset = 0; loadWallpaperFiles(); };
$('#wallpaper-previous').onclick = () => { wallpaperPicker.offset = Math.max(0, wallpaperPicker.offset - state.limit); loadWallpaperFiles(); };
$('#wallpaper-next').onclick = () => { if (wallpaperPicker.more) { wallpaperPicker.offset += state.limit; loadWallpaperFiles(); } };
async function downloadFile(link) {
  if (!state.authenticated || link.getAttribute('aria-busy') === 'true') return;
  const epoch = state.epoch;
  link.setAttribute('aria-busy', 'true'); link.setAttribute('aria-disabled', 'true');
  try {
    const blob = await api(link.getAttribute('href').slice(5), {}, false, 'blob');
    const url = URL.createObjectURL(blob), target = document.createElement('a');
    target.href = url; target.download = link.download; target.hidden = true; document.body.append(target); target.click(); target.remove();
    downloads.set(url, setTimeout(() => { URL.revokeObjectURL(url); downloads.delete(url); }, 1000));
  } catch (error) { report(error, epoch); }
  finally { link.removeAttribute('aria-busy'); link.removeAttribute('aria-disabled'); }
}
$('#refresh').onclick = loadFiles;
$('#previous').onclick = () => { state.offset = Math.max(0, state.offset - state.limit); loadFiles(); };
$('#next').onclick = () => { if (state.more) { state.offset += state.limit; loadFiles(); } };
$('#new-folder').onclick = () => { $('#folder-name').value = ''; $('#folder-dialog').showModal(); $('#folder-name').focus(); };
$('[data-close]').onclick = () => $('#folder-dialog').close();
$('#folder-form').onsubmit = async event => {
  event.preventDefault(); if (!state.authenticated) return;
  const epoch = state.epoch, name = $('#folder-name').value.trim();
  if (!name || /[\\/]/.test(name) || name === '.' || name === '..') { $('#folder-name').setCustomValidity('请输入有效的文件夹名称'); $('#folder-name').reportValidity(); return; }
  const button = $('#folder-form button[type=submit]'); button.disabled = true;
  try { await api('folders', json('POST', { path: child(name) })); $('#folder-dialog').close(); await loadFiles(); }
  catch (error) { report(error, epoch); } finally { if (current(epoch)) button.disabled = false; }
};
$('#folder-name').oninput = () => $('#folder-name').setCustomValidity('');
$('#upload').onclick = () => $('#file-input').click();
async function confirmAction(title, detail) {
  if (!state.authenticated) return false;
  const epoch = state.epoch;
  const dialog = $('#confirm-dialog'); $('#confirm-title').textContent = title; $('#confirm-detail').textContent = detail || '';
  dialog.returnValue = ''; dialog.showModal();
  return new Promise(resolve => dialog.addEventListener('close', () => resolve(current(epoch) && dialog.returnValue === 'confirm'), { once: true }));
}
function uploadFile(file, path, replace = false) {
  if (!state.authenticated) return Promise.resolve(-1);
  const epoch = state.epoch;
  return new Promise(resolve => {
    const row = el('div', undefined, 'transfer'), percent = el('span', '0%', 'transfer-percent'), cancel = el('button', '取消');
    const progress = document.createElement('progress'); progress.max = 100; progress.value = 0;
    row.append(el('span', file.name, 'transfer-name'), percent, cancel, progress); $('#transfers').append(row);
    const request = new XMLHttpRequest(); request.open('PUT', '/api/file?' + new URLSearchParams({ path, replace: replace ? '1' : '0' }));
    uploads.add(request);
    request.setRequestHeader('Content-Type', 'application/octet-stream');
    request.setRequestHeader('X-CSRF-Token', state.csrf);
    request.upload.onprogress = event => { if (event.lengthComputable) { const value = Math.floor(100 * event.loaded / event.total); progress.value = value; percent.textContent = value === 100 ? '保存中' : value + '%'; } };
    cancel.onclick = () => request.abort();
    const finish = status => { uploads.delete(request); row.remove(); resolve(current(epoch) ? status : -1); };
    request.onload = () => { if (current(epoch) && request.status === 401) endSession('登录已失效，请重新登录。上传已停止，请登录后重新选择文件。'); finish(request.status); };
    request.onerror = request.ontimeout = () => finish(0);
    request.onabort = () => finish(-1);
    request.send(file);
  });
}
$('#file-input').onchange = async () => {
  const epoch = state.epoch, files = Array.from($('#file-input').files), directory = state.path; $('#file-input').value = '';
  for (const file of files) {
    if (!current(epoch)) break;
    const path = (directory === '/' ? '/' : directory + '/') + file.name;
    let status = await uploadFile(file, path);
    if (!current(epoch)) break;
    if (status === 409 && await confirmAction('替换同名文件？', file.name)) status = await uploadFile(file, path, true);
    if (!current(epoch)) break;
    if (status >= 200 && status < 300) continue;
    if (status !== -1) notice(status ? failures[status] || '上传失败' : '连接中断');
    if (!status || status === 401) break;
  }
  if (current(epoch)) loadFiles();
};
async function loadInfo() {
  if (!state.authenticated) return;
  const generation = ++state.info;
  try {
    const info = await api('info'); if (generation !== state.info) return;
    $('#device-name').textContent = $('#info-name').textContent = info.name;
    const rows = [['电池电量', info.battery.percent === null ? '—' : info.battery.percent + '%' + (info.battery.charging ? ' · 充电中' : '')], ['运行时间', Math.floor(info.uptime / 60) + ' 小时 ' + info.uptime % 60 + ' 分钟'], ['TF 卡容量', info.storage.mounted ? bytes(info.storage.total) : '未插入'], ['TF 卡可用空间', info.storage.mounted ? bytes(info.storage.total - info.storage.used) : '—'], ['WLAN', info.wifi.ssid || '未连接'], ['IP 地址', (info.addresses || []).join(' / ') || '—'], ['WLAN 地址', info.mac || '—'], ['蓝牙地址', info.bluetooth || '—'], ['系统版本', info.version || '—'], ['SDK 版本', info.sdk || '—'], ['构建日期', info.build_date || '—']];
    (info.memory || []).forEach((ram,index) => rows.push([index ? 'PSRAM 可用空间' : 'SRAM 可用空间', bytes(ram.available) + ' / ' + bytes(ram.total)]));
    $('#device-details').replaceChildren(...rows.map(([key, value]) => { const list = el('dl', undefined, 'detail'); list.append(el('dt', key), el('dd', value)); return list; }));
    capacity(info.storage);
  } catch (error) { if (generation === state.info) report(error); }
}
$('#refresh-info').onclick = loadInfo;
function syncSettingsBusy() {
  const root = $('#settings-groups');
  root.inert = false; root.setAttribute('aria-busy', String(state.saving));
  root.querySelectorAll('button,input,select').forEach(node => {
    if (node.dataset.local) return;
    if (state.saving) {
      if (!settingDisabled.has(node)) settingDisabled.set(node, node.disabled);
      node.disabled = true;
    } else if (settingDisabled.has(node)) {
      node.disabled = settingDisabled.get(node); settingDisabled.delete(node);
    }
  });
}
async function updateSetting(control, value) {
  if (state.saving || !state.authenticated) return;
  const epoch = state.epoch;
  if (control.confirm && !await confirmAction(control.label, control.confirm)) { await loadSettings(); return; }
  if (!current(epoch)) return;
  state.saving = true;
  syncSettingsBusy();
  try {
    const result = await api('settings', json('POST', { id: control.id, value }));
    for (let attempt = 0; attempt < 480; ++attempt) {
      await pause(250, epoch);
      const job = await api('settings/job?' + new URLSearchParams({ ticket: result.ticket }));
      if (job.state === 'pending') continue;
      if (job.state !== 'done') throw new Error(job.message || '设置未保存');
      if (job.confirmation) {
        state.saving = false;
        if (await confirmAction(control.label, job.message)) await updateSetting({id:'reset_confirm',label:control.label}, job.confirmation);
        return;
      }
      if (job.disconnect) { notice('重置请求已提交'); return; }
      await loadSettings(); if (control.id === 'name') await loadInfo(); return;
    }
    notice('操作尚未完成');
  } catch (error) { report(error, epoch); if (current(epoch)) await loadSettings(); }
  finally { if (current(epoch)) { state.saving = false; syncSettingsBusy(); } }
}
async function edit(title, fields, submit = '保存') {
  if (!state.authenticated) return null;
  const epoch = state.epoch;
  const dialog = $('#edit-dialog'), form = $('#edit-form');
  $('#edit-title').textContent = title; $('#edit-fields').replaceChildren();
  form.querySelector('[type=submit]').textContent = submit;
  for (const field of fields) {
    const row = el('div', undefined, 'edit-field'), label = el('label', field.label);
    let input;
    if (field.options) {
      input = document.createElement('select');
      field.options.forEach(option => { const node = el('option', option.label); node.value = option.value; input.append(node); });
    } else { input = document.createElement('input'); input.type = field.type || 'text'; input.required = field.required !== false; }
    input.name = field.id; input.id = 'edit-' + field.id; input.value = field.value ?? ''; input.autocomplete = 'off';
    label.htmlFor = input.id; row.append(label, input); $('#edit-fields').append(row);
  }
  return new Promise(resolve => {
    let result = null;
    form.onsubmit = event => { event.preventDefault(); result = Object.fromEntries(new FormData(form)); dialog.close(); };
    $('#edit-cancel').onclick = () => dialog.close();
    dialog.addEventListener('close', () => resolve(current(epoch) ? result : null), {once:true}); dialog.showModal();
  });
}
function actionButton(label, handler) { const button = el('button', label); button.type = 'button'; button.onclick = handler; return button; }
function rememberPackage() {
  try {
    if (packageJob) sessionStorage.setItem(packageStorageKey, JSON.stringify({ id: packageJob.id, name: packageJob.name, volume: packageJob.volume, username: state.username }));
    else sessionStorage.removeItem(packageStorageKey);
  } catch (_) { /* Browsers may disable session storage. */ }
}
function drawPackage() {
  if (!packagePanel) return;
  const job = packageJob, busy = job && !job.finished;
  packagePanel.querySelectorAll('input,select,[type=submit]').forEach(node => node.disabled = !!busy);
  const status = $('.package-status', packagePanel), progress = $('progress', packagePanel), retry = $('.package-retry', packagePanel);
  status.textContent = job ? job.name + ' · ' + (job.volume === 0 ? '内部存储' : 'TF 卡') + '：' + job.text : '';
  progress.hidden = !job || !!job.finished;
  if (job && Number.isFinite(job.percent)) progress.value = job.percent; else progress.removeAttribute('value');
  retry.hidden = !job?.retry;
  $('.package-cancel', packagePanel).hidden = !job?.uploading;
}
function packageResult(job) {
  if (!packageJob) return;
  packageJob.uploading = false; packageJob.retry = false;
  if (job.state === 'pending') {
    const phase = { preparing: '等待设备开始安装', uploading: '设备正在接收安装包' }[job.phase] || job.phase || '正在安装';
    const total = Number(job.total_bytes), completed = Number(job.completed_bytes);
    packageJob.percent = total > 0 ? Math.max(0, Math.min(100, Math.floor(100 * completed / total))) : null;
    packageJob.text = phase + (total > 0 ? '，' + bytes(completed) + ' / ' + bytes(total) + '（' + packageJob.percent + '%）' : '');
  } else if (job.state === 'done' || job.state === 'failed') {
    packageJob.finished = !job.cleanup_pending;
    packageJob.text = (job.state === 'done' ? '安装完成：' : '安装失败：') + (job.message || (job.state === 'done' ? '应用已安装。' : '设备未提供失败原因。'));
    if (job.cleanup_pending) packageJob.text += ' 临时安装包正在清理。';
    else if (job.cleanup_failed) {
      const reasons = { unlink_failed: '临时安装包未能删除。', worker_unavailable: '临时安装包清理未能启动。', storage_unavailable: '存储暂不可用，临时安装包未完成清理。', session_changed: 'TF 卡已变化，原卡上的临时安装包未完成清理。' };
      packageJob.text += ' ' + (reasons[job.cleanup_error] || '临时安装包未完成清理。');
      if (job.cleanup_path) packageJob.text += ' 文件位置：' + job.cleanup_path + '。';
      packageJob.text += ' 可以继续安装其他应用。';
    }
    if (packageJob.finished) try { sessionStorage.removeItem(packageStorageKey); } catch (_) {}
  } else throw new Error('设备返回了未知安装状态，请重新查询结果。');
  drawPackage();
}
async function pollPackage() {
  if (!packageJob || packagePolling || !state.authenticated) return;
  const epoch = state.epoch, job = packageJob;
  packagePolling = true; job.retry = false; drawPackage();
  try {
    while (current(epoch) && packageJob === job && !job.finished) {
      const result = await api('package?' + new URLSearchParams({ upload: job.id }));
      packageResult(result);
      if (!job.finished) await pause(500, epoch);
    }
    if (current(epoch) && job.finished) await loadSettings();
  } catch (error) {
    if (current(epoch) && packageJob === job) {
      job.uploading = false;
      if (error.status === 404) {
        job.finished = true; job.text = '设备已无此上传任务记录。请检查已安装应用，确认结果后再上传。';
        try { sessionStorage.removeItem(packageStorageKey); } catch (_) {}
      } else {
        job.text = error.message + ' 安装结果尚未确认，可重新查询。'; job.retry = true;
      }
      drawPackage();
    }
  } finally { if (current(epoch)) packagePolling = false; }
}
async function uploadPackage(file, volume) {
  if (!state.authenticated || state.saving || packageJob && !packageJob.finished) return;
  if (!/\.sap$/i.test(file.name) || !file.size || file.size > 1073741824) { notice('请选择非空的 .sap 安装包，大小不能超过 1 GiB。'); return; }
  const epoch = state.epoch, random = new Uint8Array(16); crypto.getRandomValues(random);
  packageJob = { id: Array.from(random, value => value.toString(16).padStart(2, '0')).join(''), name: file.name, volume, uploading: true, text: '正在上传，0%', percent: 0 };
  const job = packageJob;
  rememberPackage(); drawPackage();
  try {
    const result = await new Promise((resolve, reject) => {
      const request = new XMLHttpRequest(); job.request = request; uploads.add(request);
      request.open('PUT', '/api/package?' + new URLSearchParams({ upload: job.id, volume }));
      request.withCredentials = true; request.setRequestHeader('X-CSRF-Token', state.csrf);
      request.setRequestHeader('Content-Type', 'application/octet-stream');
      const finish = (error, result) => { uploads.delete(request); job.request = null; error ? reject(error) : resolve(result); };
      request.upload.onprogress = event => {
        if (!current(epoch) || !event.lengthComputable) return;
        job.percent = Math.floor(100 * event.loaded / event.total);
        job.text = job.percent === 100 ? '上传完成，设备正在保存并提交安装' : '正在上传，' + bytes(event.loaded) + ' / ' + bytes(event.total) + '（' + job.percent + '%）';
        if (job.percent === 100) job.uploading = false;
        drawPackage();
      };
      request.onload = () => {
        if (!current(epoch)) { finish(cancelled()); return; }
        let result = null; try { result = JSON.parse(request.responseText); } catch (_) {}
        if (request.status === 401 || result?.error === 'csrf_invalid') {
          endSession('登录状态已失效，请重新登录后查询安装结果。'); finish(cancelled()); return;
        }
        if (request.status >= 200 && request.status < 300 && result) finish(null, result);
        else {
          const error = new Error(result?.message || authFailures[result?.error] || failures[request.status] || '上传响应无效');
          error.status = request.status; error.result = result; finish(error);
        }
      };
      request.onerror = () => finish(new Error('上传连接中断。'));
      request.onabort = () => finish(new Error('已停止上传，正在确认设备状态。'));
      request.ontimeout = () => finish(new Error('上传连接超时。'));
      request.send(file);
    });
    if (current(epoch)) { packageResult(result); await pollPackage(); }
  } catch (error) {
    if (!current(epoch) || packageJob !== job) return;
    job.uploading = false;
    if (error.result?.state === 'failed') { packageResult(error.result); if (!job.finished) await pollPackage(); }
    else if (error.status >= 400 && error.status !== 500) {
      job.finished = true; job.text = '上传未被接受：' + error.message;
      try { sessionStorage.removeItem(packageStorageKey); } catch (_) {}
      drawPackage();
    } else { job.text = error.message; drawPackage(); await pollPackage(); }
  }
}
function packageControls() {
  if (!packagePanel) {
    packagePanel = el('form', undefined, 'package-upload');
    const fileLabel = el('label', '电脑上的 .sap 安装包'), file = el('input');
    file.id = 'package-file'; file.type = 'file'; file.accept = '.sap'; file.required = true; fileLabel.htmlFor = file.id;
    const volumeLabel = el('label', '安装位置'), volume = el('select'); volume.id = 'package-volume'; volumeLabel.htmlFor = volume.id;
    for (const [value, label] of [[0, '内部存储'], [1, 'TF 卡']]) { const option = el('option', label); option.value = value; volume.append(option); }
    const submit = el('button', '上传并安装', 'primary'); submit.type = 'submit';
    const status = el('p', '', 'package-status'); status.setAttribute('role', 'status'); status.setAttribute('aria-live', 'polite');
    const progress = el('progress'); progress.max = 100; progress.hidden = true; progress.setAttribute('aria-label', '上传或当前安装阶段进度');
    const retry = el('button', '查询安装结果', 'package-retry'); retry.type = 'button'; retry.hidden = true; retry.onclick = pollPackage;
    const cancel = el('button', '停止上传', 'package-cancel'); cancel.type = 'button'; cancel.hidden = true; cancel.onclick = () => packageJob?.request?.abort();
    const fileField = el('div', undefined, 'package-field'), volumeField = el('div', undefined, 'package-field');
    fileField.append(fileLabel, file); volumeField.append(volumeLabel, volume);
    packagePanel.append(fileField, volumeField, el('p', '更新应用时，请选择原安装位置。', 'package-help'), submit, status, progress, cancel, retry);
    packagePanel.onsubmit = event => { event.preventDefault(); if (file.files[0]) uploadPackage(file.files[0], Number(volume.value)); };
    if (!packageJob) {
      try {
        const saved = JSON.parse(sessionStorage.getItem(packageStorageKey));
        if (saved && /^[a-f0-9]{32}$/.test(saved.id) && saved.username === state.username && [0, 1].includes(saved.volume)) {
          packageJob = { ...saved, text: '正在查询上次安装结果' }; pollPackage();
        }
      } catch (_) {}
    }
  }
  drawPackage(); return packagePanel;
}
const fontPhases = { idle: '就绪', queued: '等待设备执行', receiving: '设备正在接收字体', copying: '正在复制字体', validating: '正在校验字体', committing: '正在保存字体', done: '任务已完成', failed: '任务失败' };
function fontPending(data) { return !!data && (data.busy || ['queued', 'receiving', 'copying', 'validating', 'committing'].includes(data.phase)); }
function fontStorage(font) { return { builtin: '内置', sd: 'TF 卡', internal: '内部存储' }[font.storage] || '未知位置'; }
function fontBusy() { return readingFonts.sending || readingFonts.uploading || fontPending(readingFonts.data) || !!readingFonts.job && !readingFonts.job.finished; }
function drawFonts() {
  const data = readingFonts.data, busy = fontBusy();
  $('#font-upload').querySelectorAll('input,select,button').forEach(node => node.disabled = busy || !data?.ready);
  $('#font-refresh').disabled = readingFonts.uploading || readingFonts.sending;
  $('#font-cancel').hidden = !readingFonts.uploading || !readingFonts.request || readingFonts.percent === 100;
  $('#font-status').textContent = readingFonts.text;
  const progress = $('#font-progress');
  progress.hidden = !busy;
  if (Number.isFinite(readingFonts.percent)) progress.value = readingFonts.percent; else progress.removeAttribute('value');
  if (!data) return;
  const selected = data.fonts.find(font => font.id === data.selected), preferred = data.fonts.find(font => font.id === data.preferred);
  $('#font-current').textContent = '当前实际字体：' + (selected?.name || '暂未取得字体信息') + (data.preferred !== data.selected ? '。已保存偏好：' + (preferred?.name || '暂不可用的字体') + '；可用后在下次打开阅读时恢复，也可主动应用。' : '。');
  const rows = data.fonts.map(font => {
    const expanded = readingFonts.detail === font.id;
    const row = el('div', undefined, 'font-row'), name = el('div'), button = el('button', expanded ? '收起详情' : '详情');
    row.setAttribute('aria-current', font.selected ? 'true' : 'false');
    name.append(el('span', font.name), el('p', [fontStorage(font), font.id ? bytes(font.size) : '默认字体', font.selected ? '正在使用' : font.preferred ? '已保存偏好' : '', font.available ? '' : '暂不可用'].filter(Boolean).join(' · '), 'font-meta'));
    button.type = 'button'; button.setAttribute('aria-expanded', String(expanded)); button.setAttribute('aria-controls', 'font-detail');
    button.onclick = () => {
      readingFonts.detail = readingFonts.detail === font.id ? null : font.id;
      drawFonts();
      if (readingFonts.detail !== null) $('#font-detail').scrollIntoView({ block: 'nearest' });
    };
    row.append(name, button); return row;
  });
  $('#font-list').replaceChildren(...rows);
  const font = data.fonts.find(item => item.id === readingFonts.detail), detail = $('#font-detail');
  detail.replaceChildren(); detail.hidden = !font;
  if (!font) return;
  detail.append(el('h3', font.name), el('p', fontStorage(font) + (font.id ? ' · ' + bytes(font.size) : ' · 平台默认阅读字体')),
    el('p', '可在设备的“阅读设置 → 字体”中预览效果。'));
  if (!font.available) detail.append(el('p', '字体暂不可用，请检查对应存储后刷新列表。'));
  const actions = el('div', undefined, 'collection-actions'), apply = el('button', font.selected && font.preferred ? '正在使用此字体' : '使用此字体', 'primary');
  apply.type = 'button'; apply.disabled = busy || !data.ready || !font.available || font.selected && font.preferred;
  apply.onclick = () => changeFont('select', font); actions.append(apply);
  if (font.id !== 0) {
    const remove = el('button', '删除字体'); remove.type = 'button'; remove.disabled = busy || !data.ready || !font.available;
    remove.onclick = () => changeFont('delete', font); actions.append(remove);
  }
  detail.append(actions);
}
function fontResult(data, result, unavailable = false) {
  readingFonts.data = data; readingFonts.percent = null;
  const job = readingFonts.job;
  if (readingFonts.detail !== null && !data.fonts.some(font => font.id === readingFonts.detail)) readingFonts.detail = null;
  if (job && !job.finished) {
    if (unavailable) {
      job.finished = true; job.text = '任务记录已不可用，上次操作结果尚未确认，请检查字体列表和当前实际字体。';
    } else if (result.phase === 'done' && !result.error) {
      job.finished = true;
      job.text = { upload: '字体安装完成。点击“使用此字体”后才会应用到阅读正文。', select: '阅读字体应用完成。', delete: '字体删除完成。' }[job.action];
    } else if (result.phase === 'failed') {
      job.finished = true; job.text = '字体操作失败：' + (result.message || '设备错误 ' + result.error);
    }
  }
  const progress = job && !job.finished ? result : data;
  if (job?.finished) readingFonts.text = job.text + (fontPending(data) ? ' 设备正在处理其他字体任务。' : '');
  else if (fontPending(progress)) {
    const total = Number(progress.total), completed = Number(progress.completed);
    readingFonts.text = (fontPhases[progress.phase] || '正在处理字体任务') + (total > 0 && ['receiving', 'copying'].includes(progress.phase) ? '，' + bytes(completed) + ' / ' + bytes(total) : '');
    if (total > 0 && ['receiving', 'copying'].includes(progress.phase)) readingFonts.percent = Math.min(100, Math.max(0, completed / total * 100));
  }
  else if (!data.ready) readingFonts.text = '字体服务尚未就绪，请稍后刷新。';
  else if (data.phase === 'failed') readingFonts.text = '最近的字体任务失败：' + (data.message || '设备错误 ' + data.error);
  else readingFonts.text = data.phase === 'done' ? '最近的字体任务已完成。' : '选择字体查看详情，点击“使用此字体”后生效。';
}
async function loadFonts() {
  if (!state.authenticated || readingFonts.uploading || readingFonts.sending) return;
  clearTimeout(readingFonts.timer);
  const epoch = state.epoch, generation = ++readingFonts.generation;
  try {
    const job = readingFonts.job;
    let result = null, unavailable = false;
    if (job && !job.finished) {
      try { result = await api('fonts/status?' + new URLSearchParams({ ticket: job.ticket })); }
      catch (error) { if (error.status === 404) unavailable = true; else throw error; }
      if (!current(epoch) || generation !== readingFonts.generation) return;
      if (!unavailable && (!result || result.ticket !== job.ticket || !(result.phase in fontPhases))) throw new Error('字体任务响应无效，请重新查询。');
    }
    const catalog = await api('fonts/catalog');
    if (!current(epoch) || generation !== readingFonts.generation) return;
    if (!Array.isArray(catalog.fonts) || !(catalog.phase in fontPhases)) throw new Error('字体状态响应无效，请刷新后重试。');
    fontResult(catalog, result, unavailable); drawFonts();
    if (fontPending(catalog) || readingFonts.job && !readingFonts.job.finished) readingFonts.timer = setTimeout(loadFonts, 900);
  } catch (error) {
    if (!current(epoch) || generation !== readingFonts.generation) return;
    readingFonts.text = error.message + ' 字体任务结果尚未确认，请刷新字体与任务状态。'; drawFonts();
  }
}
async function changeFont(action, font) {
  if (!state.authenticated || fontBusy()) return;
  const epoch = state.epoch;
  let confirmed = false;
  if (action === 'delete') {
    const active = font.selected || font.preferred;
    if (!await confirmAction('删除“' + font.name + '”？', active ? '此字体正在使用或已保存为阅读偏好。设备将先改用内置 MiSans，再删除字体文件。' : '设备将删除此字体文件。需要使用时，可以重新安装。')) return;
    confirmed = active;
  }
  if (!current(epoch) || fontBusy()) return;
  clearTimeout(readingFonts.timer); ++readingFonts.generation;
  readingFonts.sending = true; readingFonts.text = '正在提交字体操作…'; readingFonts.percent = null; drawFonts();
  try {
    const result = await api('fonts/' + action, json('POST', { id: font.id, ...(action === 'delete' ? { confirm_active: confirmed } : {}) }));
    if (!current(epoch)) return;
    if (!result.accepted || !Number.isInteger(result.ticket) || result.ticket <= 0) throw new Error('设备尚未确认受理字体操作。');
    readingFonts.job = { ticket: result.ticket, action, finished: false };
    readingFonts.text = '请求已受理，等待设备执行。';
  } catch (error) {
    if (!current(epoch)) return;
    readingFonts.job = null; readingFonts.text = error.message + ' 请刷新字体与任务状态确认结果。';
  } finally { if (current(epoch)) { readingFonts.sending = false; drawFonts(); } }
  if (current(epoch) && readingFonts.job) await loadFonts();
}
async function uploadFont(file, storage) {
  if (!state.authenticated || fontBusy()) return;
  if (!/\.ttf$/i.test(file.name) || !file.size || file.size > 32 * 1024 * 1024) { notice('请选择非空的静态 TrueType .ttf 字体，大小不能超过 32 MiB。'); return; }
  const epoch = state.epoch;
  clearTimeout(readingFonts.timer); ++readingFonts.generation;
  readingFonts.uploading = true; readingFonts.job = null; readingFonts.text = '正在上传字体，0%'; readingFonts.percent = 0; drawFonts();
  try {
    const result = await new Promise((resolve, reject) => {
      const request = new XMLHttpRequest(); readingFonts.request = request; uploads.add(request);
      request.open('PUT', '/api/font?' + new URLSearchParams({ name: file.name, storage }));
      request.withCredentials = true; request.timeout = 15 * 60 * 1000;
      request.setRequestHeader('X-CSRF-Token', state.csrf); request.setRequestHeader('Content-Type', 'application/octet-stream');
      const finish = (error, result) => { uploads.delete(request); if (readingFonts.request === request) readingFonts.request = null; error ? reject(error) : resolve(result); };
      request.upload.onprogress = event => {
        if (!current(epoch) || !event.lengthComputable) return;
        readingFonts.percent = Math.floor(100 * event.loaded / event.total);
        readingFonts.text = readingFonts.percent === 100 ? '字节已传输，等待设备保存并受理安装…' : '正在上传字体，' + bytes(event.loaded) + ' / ' + bytes(event.total);
        drawFonts();
      };
      request.onload = () => {
        if (!current(epoch)) { finish(cancelled()); return; }
        let result = null; try { result = JSON.parse(request.responseText); } catch (_) {}
        if (request.status === 401 || result?.error === 'csrf_invalid') { endSession('登录已失效，请重新登录后查询字体状态。'); finish(cancelled()); return; }
        if (request.status === 202 && result?.accepted && Number.isInteger(result.ticket) && result.ticket > 0) finish(null, result);
        else finish(new Error(result?.message || authFailures[result?.error] || failures[request.status] || '设备尚未确认受理安装。'));
      };
      request.onerror = () => finish(new Error('字体上传连接中断。'));
      request.onabort = () => finish(new Error('已停止字体传输。'));
      request.ontimeout = () => finish(new Error('字体上传连接超时。'));
      request.send(file); drawFonts();
    });
    if (!current(epoch)) return;
    readingFonts.job = { ticket: result.ticket, action: 'upload', finished: false };
    readingFonts.text = '上传已受理，等待设备校验和安装。'; readingFonts.percent = null;
  } catch (error) {
    if (!current(epoch)) return;
    readingFonts.text = error.message + ' 安装结果尚未确认，请刷新字体列表。'; readingFonts.percent = null;
  } finally { if (current(epoch)) { readingFonts.uploading = false; drawFonts(); } }
  if (current(epoch) && readingFonts.job) await loadFonts();
}
$('#font-upload').onsubmit = event => { event.preventDefault(); const file = $('#font-file').files[0]; if (file) uploadFont(file, $('#font-storage').value); };
$('#font-refresh').onclick = loadFonts;
$('#font-cancel').onclick = () => readingFonts.request?.abort();

const aiHeaders = ['anthropic-version', 'anthropic-beta', 'OpenAI-Organization', 'OpenAI-Project', 'OpenAI-Beta'];
const aiTimeouts = [['connect_ms', '连接超时', 30000], ['send_idle_ms', '发送空闲超时', 30000], ['first_event_ms', '首个事件超时', 90000], ['read_idle_ms', '读取空闲超时', 60000], ['total_ms', '总超时', 300000]];
const speechCredentials = {
  tencent: [['app_id', 'AppID', 64], ['secret_id', 'SecretId', 128], ['secret', 'SecretKey', 512]],
  ali: [['workspace', 'WorkspaceId', 64], ['secret', 'API Key', 512]],
  doubao: [['secret', 'API Key', 512]]
};
function serviceMessage(kind, message) {
  serviceConfigs[kind].message = message;
  $('#' + kind + '-status').textContent = message;
}
function serviceBlocked(kind) {
  const service = serviceConfigs[kind];
  return !state.authenticated || !service.data?.initialized || service.data.busy || service.sending || !!service.job;
}
function syncServiceBusy(kind) {
  const root = $('#' + kind + '-settings'), busy = serviceBlocked(kind);
  root.setAttribute('aria-busy', String(serviceConfigs[kind].sending || !!serviceConfigs[kind].job || !!serviceConfigs[kind].data?.busy));
  root.querySelectorAll('button,input,select').forEach(node => {
    if (node.dataset.local) return;
    if (busy) {
      if (!serviceDisabled.has(node)) serviceDisabled.set(node, node.disabled);
      node.disabled = true;
    } else if (serviceDisabled.has(node)) {
      node.disabled = serviceDisabled.get(node); serviceDisabled.delete(node);
    }
  });
}
function closeServiceEditor(kind) {
  const service = serviceConfigs[kind];
  service.editor?.form.querySelectorAll('input').forEach(input => { if (input.type === 'password') input.value = ''; });
  service.editor = null;
  const host = $('#' + kind + '-editor'); host.replaceChildren(); host.hidden = true;
}
function clearServiceConfigs() {
  serviceApplications = [];
  for (const kind of ['ai', 'speech']) {
    closeServiceEditor(kind);
    const service = serviceConfigs[kind]; ++service.generation;
    Object.assign(service, { data: null, sending: false, job: null, message: '' });
    $('#' + kind + '-list').replaceChildren(); $('#' + kind + '-status').textContent = '';
    $('#' + kind + '-settings').removeAttribute('aria-busy');
  }
}
async function replaceServiceEditor(kind) {
  const epoch = state.epoch, editor = serviceConfigs[kind].editor;
  if (serviceBlocked(kind)) return false;
  if (editor?.dirty && !await confirmAction('放弃未保存的修改？', '当前表单的修改尚未保存。')) return false;
  if (!current(epoch) || serviceBlocked(kind) || serviceConfigs[kind].editor !== editor) return false;
  closeServiceEditor(kind); return true;
}
function serviceField(parent, label, name, value = '', options = {}) {
  const wrap = el('label', undefined, 'service-field'), input = el(options.choices ? 'select' : 'input');
  input.id = 'service-field-' + ++serviceFieldId; input.name = name; wrap.htmlFor = input.id;
  if (options.choices) for (const [id, title] of options.choices) { const option = el('option', title); option.value = String(id); input.append(option); }
  else input.type = options.type || 'text';
  if (input.type === 'checkbox') input.checked = !!value;
  else input.value = String(value ?? '');
  for (const key of ['min', 'max', 'step', 'maxLength', 'placeholder', 'readOnly', 'required']) if (key in options) input[key] = options[key];
  if (input.type === 'password') { input.autocomplete = 'new-password'; input.spellcheck = false; input.setAttribute('autocapitalize', 'none'); }
  wrap.append(el('span', label), input); parent.append(wrap); return input;
}
function serviceDetails(parent, title) { const details = el('details', undefined, 'service-details'); details.append(el('summary', title)); parent.append(details); return details; }
function serviceBits(parent, title, name, value, labels, dependencies = []) {
  const group = el('fieldset', undefined, 'service-bits'); group.append(el('legend', title)); parent.append(group);
  const inputs = labels.map(([bit, label]) => [bit, serviceField(group, label, name + '-' + bit, !!(value & bit), { type: 'checkbox' })]);
  const applyDependencies = () => {
    for (const [requiredBit, input] of inputs) {
      const owners = dependencies.filter(([, bit]) => bit === requiredBit);
      if (!owners.length) continue;
      const required = owners.some(([bit]) => inputs.some(([candidate, control]) => candidate === bit && control.checked));
      if (required) input.checked = true;
      input.disabled = required;
    }
  };
  inputs.forEach(([, input]) => input.addEventListener('change', applyDependencies));
  applyDependencies();
  return () => inputs.reduce((bits, [bit, input]) => bits | (input.checked ? bit : 0), 0);
}
function serviceText(value, label, limit, optional = false) {
  if ((!optional && !value) || new TextEncoder().encode(value).length >= limit || /[\x00-\x1f\x7f]/.test(value)) throw new Error(label + '不能为空、包含控制字符或达到 ' + limit + ' 字节。');
  return value;
}
function serviceNumber(input, label, min, max, integer = true) {
  const value = Number(input.value);
  if (input.value === '' || !Number.isFinite(value) || value < min || value > max || integer && !Number.isInteger(value)) throw new Error(label + '必须为 ' + min + ' 到 ' + max + ' 之间的' + (integer ? '整数。' : '数值。'));
  return value;
}
function serviceForm(kind, title) {
  const form = el('form', undefined, 'service-form'), host = $('#' + kind + '-editor');
  form.noValidate = true; form.autocomplete = 'off'; form.append(el('h3', title));
  const editor = { form, dirty: false };
  form.addEventListener('input', () => editor.dirty = true);
  form.addEventListener('change', () => editor.dirty = true);
  serviceConfigs[kind].editor = editor; host.replaceChildren(form); host.hidden = false; return editor;
}
function serviceFormActions(kind, editor, payload) {
  const bar = el('div', undefined, 'service-actions'), submit = el('button', '保存配置', 'primary'); submit.type = 'submit';
  const cancel = actionButton('取消编辑', async () => { if (await replaceServiceEditor(kind)) serviceMessage(kind, '已取消编辑。'); });
  bar.append(submit, cancel); editor.form.append(bar);
  editor.form.onsubmit = async event => {
    event.preventDefault(); if (serviceBlocked(kind) || serviceConfigs[kind].editor !== editor) return;
    const epoch = state.epoch;
    try { await changeService(kind, payload(), '配置已保存。', editor); }
    catch (error) { if (current(epoch)) serviceMessage(kind, error.message); }
  };
}
function publicAiProfile(profile) {
  return {
    id: profile.id, name: profile.name, protocol: profile.protocol, enabled: profile.enabled, is_default: profile.is_default,
    endpoint: profile.endpoint, auth: profile.auth, features: profile.features,
    models: profile.models.map(model => ({ id: model.id, capabilities: model.capabilities, max_output_tokens: model.max_output_tokens, token_field: model.token_field })),
    headers: profile.headers.map(header => ({ name: header.name, value: header.value })),
    ...Object.fromEntries(aiTimeouts.map(([key]) => [key, profile[key]]))
  };
}
async function loadServiceConfig(kind) {
  const service = serviceConfigs[kind];
  if (!state.authenticated || service.sending) return;
  if (service.job) return resumeServiceJob(kind);
  const epoch = state.epoch, generation = ++service.generation;
  try {
    const result = await api(kind + '/config');
    if (!current(epoch) || generation !== service.generation) return;
    if (typeof result.initialized !== 'boolean' || !Array.isArray(result[kind === 'ai' ? 'profiles' : 'providers'])) throw new Error('服务配置响应无效，请刷新后重试。');
    service.data = kind === 'ai' ? {
      initialized: result.initialized, busy: result.busy, default_id: result.default_id,
      profiles: result.profiles.map(profile => ({ ...publicAiProfile(profile), has_secret: !!profile.has_secret })),
      grants: (result.grants || []).map(grant => ({ app: grant.app, profile: grant.profile }))
    } : {
      initialized: result.initialized, busy: result.busy, asr: result.asr, tts: result.tts,
      providers: result.providers.filter(provider => Object.hasOwn(speechCredentials, provider.id)).map(provider => ({
        id: provider.id, name: provider.name, configured: provider.configured, enabled: provider.enabled,
        voice: provider.voice, rate: provider.rate, asr: provider.asr, tts: provider.tts,
        voices: (provider.voices || []).map(voice => ({ id: voice.id, name: voice.name }))
      }))
    };
    if (kind === 'ai') drawAiConfig(); else drawSpeechConfig();
    $('#' + kind + '-status').textContent = !result.initialized ? '服务尚未就绪，请稍后刷新。' : result.busy ? '设备正在处理配置，请稍后刷新。' : service.message || '选择服务进行配置。';
    syncServiceBusy(kind);
  } catch (error) { if (current(epoch) && generation === service.generation) serviceMessage(kind, error.message); }
}
async function pollServiceJob(kind, epoch) {
  const service = serviceConfigs[kind], job = service.job;
  for (let attempt = 0; attempt < 300; ++attempt) {
    await pause(400, epoch);
    if (!current(epoch)) return;
    let result;
    try { result = await api(kind + '/config/job?' + new URLSearchParams({ ticket: job.ticket })); }
    catch (error) {
      if (!current(epoch) || service.job !== job) return;
      if (error.status !== 404) throw error;
      service.job = null;
      serviceMessage(kind, '任务记录已失效，请核对当前配置后再操作');
      return;
    }
    if (!current(epoch) || service.job !== job) return;
    if (result.state === 'pending') continue;
    if (result.state === 'failed') { service.job = null; throw new Error(result.message || '配置操作失败，请检查后重试。'); }
    if (result.state !== 'done') throw new Error('配置任务响应无效。');
    service.job = null;
    if (job.editor && service.editor === job.editor) closeServiceEditor(kind);
    serviceMessage(kind, job.success); return;
  }
  throw new Error('配置任务仍在处理中。');
}
async function resumeServiceJob(kind) {
  const service = serviceConfigs[kind], epoch = state.epoch;
  if (!current(epoch) || service.sending || !service.job) return;
  service.sending = true; ++service.generation; syncServiceBusy(kind);
  serviceMessage(kind, '正在查询配置任务…');
  try { await pollServiceJob(kind, epoch); }
  catch (error) { if (current(epoch)) serviceMessage(kind, error.message + (service.job ? ' 操作结果尚未确认，请刷新设置继续查询。' : '')); }
  finally { if (current(epoch)) { service.sending = false; syncServiceBusy(kind); } }
  if (current(epoch) && !service.job) await loadServiceConfig(kind);
}
async function changeService(kind, payload, success, editor = null) {
  if (serviceBlocked(kind)) return;
  const request = json('POST', payload);
  if (new TextEncoder().encode(request.body).length > 16384) throw new Error('配置内容超过 16 KiB，请减少扩展参数后重试。');
  const service = serviceConfigs[kind], epoch = state.epoch;
  service.sending = true; ++service.generation; syncServiceBusy(kind); serviceMessage(kind, '正在提交配置…');
  try {
    const result = await api(kind + '/config', request);
    if (!current(epoch)) return;
    if (!Number.isSafeInteger(result.ticket) || result.ticket <= 0) throw new Error('设备未返回有效任务编号，操作结果尚未确认。');
    service.job = { ticket: result.ticket, success, editor };
    serviceMessage(kind, '请求已受理，等待设备保存…');
    await pollServiceJob(kind, epoch);
  } catch (error) { if (current(epoch)) serviceMessage(kind, error.message + (service.job ? ' 操作结果尚未确认，请刷新设置继续查询。' : '')); }
  finally { if (current(epoch)) { service.sending = false; syncServiceBusy(kind); } }
  if (current(epoch) && !service.job) await loadServiceConfig(kind);
}
async function removeService(kind, id, name) {
  const epoch = state.epoch;
  if (serviceBlocked(kind) || !await confirmAction('删除“' + name + '”配置？', '设备将删除这项配置及其已保存的凭据。')) return;
  if (!current(epoch) || serviceBlocked(kind)) return;
  const editor = serviceConfigs[kind].editor;
  await changeService(kind, { operation: 'remove', [kind === 'ai' ? 'profile_id' : 'provider']: id }, '配置已删除。', editor?.identity === id ? editor : null);
}
function aiPreset(preset) {
  const presets = {
    openai: ['OpenAI', 'https://api.openai.com/v1/responses', 0, 0, 1],
    deepseek: ['DeepSeek', 'https://api.deepseek.com/responses', 0, 0, 2],
    anthropic: ['Anthropic', 'https://api.anthropic.com/v1/messages', 2, 1, 0],
    custom: ['自定义 AI', 'https://', 0, 0, 0]
  };
  const [name, endpoint, protocol, auth, features] = presets[preset] || presets.custom;
  let index = 1; while (serviceConfigs.ai.data?.profiles.some(profile => profile.id === preset + '-' + index)) ++index;
  return { id: preset + '-' + index, name, endpoint, protocol, auth, features, enabled: true, is_default: false, models: [],
    headers: protocol === 2 ? [{ name: 'anthropic-version', value: '2023-06-01' }] : [], ...Object.fromEntries(aiTimeouts.map(([key, , value]) => [key, value])) };
}
function aiSearchConfigError(protocol, features, model) {
  if (features & 2) {
    if (protocol !== 0) return 'DeepSeek 联网搜索需要 Responses 协议，请在扩展参数中切换。';
    if (model !== 'deepseek-v4-pro') return 'DeepSeek 联网搜索需要使用 deepseek-v4-pro 模型。';
  }
  if (protocol !== 0 && protocol !== 2) return '联网搜索需要 Responses 或 Messages 协议，请在扩展参数中切换。';
  return '';
}
function drawAiConfig() {
  const data = serviceConfigs.ai.data, root = $('#ai-list'), oldPreset = root.querySelector('select')?.value;
  const oldGrants = captureServiceGrants(); root.replaceChildren();
  const bar = el('div', undefined, 'service-toolbar');
  const preset = serviceField(bar, '新增服务预设', 'ai-preset', oldPreset || 'openai', { choices: [['openai', 'OpenAI'], ['deepseek', 'DeepSeek'], ['anthropic', 'Anthropic'], ['custom', '自定义']] });
  const add = actionButton('新增 AI 服务', () => openAiEditor(null, preset.value)); add.disabled = data.profiles.length >= 8;
  bar.append(add, el('span', data.profiles.length + ' / 8 项', 'service-help')); root.append(bar);
  if (!data.profiles.length) root.append(el('p', '尚未添加 AI 服务。', 'service-help'));
  for (const profile of data.profiles) {
    const row = el('div', undefined, 'service-row'), name = el('div', undefined, 'service-name'), actions = el('div', undefined, 'service-actions');
    const isDefault = data.default_id === profile.id;
    name.append(el('strong', profile.name), el('span', (isDefault ? '默认 · ' : '') + (profile.enabled ? '已启用' : '已停用') + ' · ' + (profile.has_secret ? '已保存密钥' : '未保存密钥') + ' · ' + profile.models.length + ' 个模型', 'service-help'));
    const use = actionButton(isDefault ? '默认服务' : '设为默认', () => changeService('ai', { operation: 'default', profile_id: profile.id }, '默认 AI 服务已更新。')); use.disabled = isDefault;
    actions.append(actionButton('编辑', () => openAiEditor(profile)), use, actionButton('删除', () => removeService('ai', profile.id, profile.name)));
    row.append(name, actions); root.append(row);
  }
  const grants = el('div', undefined, 'service-grants'); grants.id = 'ai-grants'; root.append(grants); drawServiceGrants(oldGrants);
}
function captureServiceGrants() {
  const root = $('#ai-grants'), selects = root?.querySelectorAll('select');
  return { app: selects?.[0]?.value, profile: selects?.[1]?.value, open: root?.querySelector('details')?.open };
}
function drawServiceGrants(previous = captureServiceGrants()) {
  const data = serviceConfigs.ai.data, root = $('#ai-grants'); if (!data || !root) return;
  root.replaceChildren();
  const details = serviceDetails(root, '应用访问授权'); details.open = !!previous.open;
  const bar = el('div', undefined, 'service-toolbar'); details.append(bar);
  const apps = [...new Map(serviceApplications.map(app => [app.id, app])).values()];
  const application = serviceField(bar, '已安装应用', 'grant-app', apps.some(app => app.id === previous.app) ? previous.app : apps[0]?.id || '', { choices: apps.map(app => [app.id, app.name + ' · ' + app.id]) });
  const profile = serviceField(bar, 'AI 服务', 'grant-profile', data.profiles.some(item => item.id === previous.profile) ? previous.profile : data.profiles[0]?.id || '', { choices: data.profiles.map(item => [item.id, item.name]) });
  const grant = actionButton('授权访问', () => changeService('ai', { operation: 'grant', profile_id: profile.value, app_id: application.value }, '应用访问权限已更新。'));
  grant.disabled = !apps.length || !data.profiles.length; bar.append(grant);
  if (!apps.length) details.append(el('p', '暂无已安装应用，请安装应用后刷新设置。', 'service-help'));
  for (const item of data.grants) {
    const app = apps.find(app => app.id === item.app), service = data.profiles.find(profile => profile.id === item.profile);
    const row = el('div', undefined, 'service-row');
    row.append(el('span', (app?.name || item.app) + ' → ' + (service?.name || item.profile)), actionButton('撤销授权', () => changeService('ai', { operation: 'revoke', profile_id: item.profile, app_id: item.app }, '应用访问权限已撤销。'))); details.append(row);
  }
  syncServiceBusy('ai');
}
async function openAiEditor(existing, preset = 'openai') {
  if (!existing && (serviceConfigs.ai.data?.profiles.length || 0) >= 8 || !await replaceServiceEditor('ai')) return;
  const profile = existing ? publicAiProfile(existing) : aiPreset(preset), editor = serviceForm('ai', existing ? '编辑 AI 服务' : '新增 AI 服务');
  editor.identity = existing?.id;
  const form = editor.form, fields = el('div', undefined, 'service-fields'); form.append(fields);
  const name = serviceField(fields, '服务名称', 'name', profile.name, { maxLength: 63 });
  const enabled = serviceField(fields, '启用服务', 'enabled', profile.enabled, { type: 'checkbox' });
  const endpoint = serviceField(fields, '接口地址', 'endpoint', profile.endpoint, { maxLength: 511 });
  const secret = serviceField(fields, 'API 密钥', 'secret', '', { type: 'password', maxLength: 1023, placeholder: existing ? '留空保留已存密钥' : '输入 API 密钥' });
  const models = el('div', undefined, 'service-models'); form.append(el('h4', '模型'), models);
  const modelRows = [];
  const addModel = actionButton('添加模型', () => { appendModel({ id: '', capabilities: 3, max_output_tokens: 1024, token_field: 0 }); editor.dirty = true; });
  function appendModel(model) {
    if (modelRows.length >= 8) return;
    const row = el('div', undefined, 'service-model'), common = el('div', undefined, 'service-toolbar'); row.append(common);
    const id = serviceField(common, '模型 ID', 'model-id', model.id, { maxLength: 127 });
    const advanced = serviceDetails(row, '模型能力与输出限制');
    const tokens = serviceField(advanced, '最大输出 token 数', 'max_output_tokens', model.max_output_tokens, { type: 'number', min: 1, max: 4294967295, step: 1 });
    const tokenField = serviceField(advanced, 'Chat 输出限制字段', 'token_field', model.token_field, { choices: [[0, 'max_tokens'], [1, 'max_completion_tokens']] });
    const capabilities = serviceBits(advanced, '支持的模型能力', 'capabilities', model.capabilities, [[1, '文本'], [2, '流式输出'], [4, '工具调用'], [8, 'JSON'], [16, 'JSON Schema'], [32, '原生续接'], [64, '联网搜索']], [[64, 32]]);
    const entry = { id, tokens, tokenField, capabilities }; modelRows.push(entry);
    common.append(actionButton('移除模型', () => { modelRows.splice(modelRows.indexOf(entry), 1); row.remove(); addModel.disabled = false; editor.dirty = true; }));
    models.append(row); addModel.disabled = modelRows.length >= 8;
  }
  profile.models.forEach(appendModel); if (!modelRows.length) appendModel({ id: '', capabilities: 3, max_output_tokens: 1024, token_field: 0 });
  form.append(addModel);
  const advanced = serviceDetails(form, '扩展参数'), extra = el('div', undefined, 'service-fields'); advanced.append(extra);
  const id = serviceField(extra, '配置 ID', 'id', profile.id, { maxLength: 31, readOnly: !!existing });
  const protocol = serviceField(extra, '接口协议', 'protocol', profile.protocol, { choices: [[0, 'OpenAI Responses'], [1, 'Chat Completions'], [2, 'Anthropic Messages']] });
  const auth = serviceField(extra, '认证方式', 'auth', profile.auth, { choices: [[0, 'Bearer'], [1, 'x-api-key']] });
  const features = serviceBits(advanced, '服务扩展', 'features', profile.features, [[1, 'OpenAI 扩展'], [2, 'DeepSeek 扩展'], [4, '加密推理内容']]);
  const headers = el('div', undefined, 'service-headers'); advanced.append(el('h4', '附加请求头'), headers);
  const headerRows = [], addHeader = actionButton('添加请求头', () => { appendHeader({ name: aiHeaders[0], value: '' }); editor.dirty = true; });
  function appendHeader(header) {
    if (headerRows.length >= 4) return;
    const row = el('div', undefined, 'service-toolbar');
    const canonical = aiHeaders.find(name => name.toLowerCase() === header.name.toLowerCase()) || header.name;
    const name = serviceField(row, '请求头名称', 'header-name', canonical, { choices: aiHeaders.map(name => [name, name]) });
    const value = serviceField(row, '请求头内容', 'header-value', header.value, { maxLength: 191 }), entry = { name, value }; headerRows.push(entry);
    row.append(actionButton('移除请求头', () => { headerRows.splice(headerRows.indexOf(entry), 1); row.remove(); addHeader.disabled = false; editor.dirty = true; }));
    headers.append(row); addHeader.disabled = headerRows.length >= 4;
  }
  profile.headers.forEach(appendHeader); advanced.append(addHeader);
  const timeouts = el('div', undefined, 'service-fields'); advanced.append(el('h4', '超时（毫秒）'), timeouts);
  const timeoutInputs = aiTimeouts.map(([key, label, fallback]) => [key, label, serviceField(timeouts, label, key, profile[key] ?? fallback, { type: 'number', min: 0, max: 900000, step: 1 })]);
  advanced.append(el('p', '超时填 0 时使用服务默认值，各阶段超时不能超过总超时。', 'service-help'));
  serviceFormActions('ai', editor, () => {
    const value = {
      id: serviceText(id.value.trim(), '配置 ID', 32), name: serviceText(name.value.trim(), '服务名称', 64),
      endpoint: serviceText(endpoint.value.trim(), '接口地址', 512), enabled: enabled.checked, is_default: !!existing && serviceConfigs.ai.data.default_id === profile.id,
      protocol: Number(protocol.value), auth: Number(auth.value), features: features(),
      models: modelRows.map(row => ({ id: serviceText(row.id.value.trim(), '模型 ID', 128), capabilities: row.capabilities(), max_output_tokens: serviceNumber(row.tokens, '最大输出 token 数', 1, 4294967295), token_field: Number(row.tokenField.value) })),
      headers: headerRows.map(row => ({ name: row.name.value, value: serviceText(row.value.value.trim(), '请求头内容', 192) })),
      ...Object.fromEntries(timeoutInputs.map(([key, label, input]) => [key, serviceNumber(input, label, 0, 900000)]))
    };
    if (!/^[a-zA-Z0-9_.-]+$/.test(value.id)) throw new Error('配置 ID 只能包含字母、数字、下划线、点和连字符。');
    if (!existing && serviceConfigs.ai.data.profiles.length >= 8) throw new Error('最多可以添加 8 个 AI 服务。');
    if (!existing && serviceConfigs.ai.data.profiles.some(item => item.id === value.id)) throw new Error('配置 ID 已存在，请换一个 ID。');
    let url; try { url = new URL(value.endpoint); } catch (_) { throw new Error('请输入完整的 HTTPS 接口地址。'); }
    if (url.protocol !== 'https:' || url.username || url.password || url.hash) throw new Error('接口地址必须使用 HTTPS，且不能包含账号、密码或片段。');
    if ([...url.searchParams.keys()].some(key => /^(key|api_key|api-key|apikey|token|access_token|authorization)$/i.test(key))) throw new Error('请将密钥填写在 API 密钥字段。');
    if (!value.models.length || value.models.length > 8 || new Set(value.models.map(model => model.id)).size !== value.models.length) throw new Error('请添加 1 到 8 个模型，模型 ID 不能重复。');
    if (value.models.some(model => !(model.capabilities & 1))) throw new Error('每个模型都必须启用文本能力。');
    for (const model of value.models) {
      const error = model.capabilities & 64 ? aiSearchConfigError(value.protocol, value.features, model.id) : '';
      if (error) throw new Error(model.id + '：' + error);
    }
    if (value.features & 2 && (value.protocol === 2 || value.features & 1)) throw new Error('DeepSeek 扩展需要 Responses 或 Chat Completions 协议，且不能同时启用 OpenAI 扩展。');
    if (value.features & 4 && value.protocol !== 0) throw new Error('加密推理内容需要 OpenAI Responses 协议。');
    if (new Set(value.headers.map(header => header.name.toLowerCase())).size !== value.headers.length) throw new Error('请求头名称不能重复。');
    for (const header of value.headers) {
      if (!aiHeaders.includes(header.name) || header.name.startsWith('anthropic-') !== (value.protocol === 2)) throw new Error('请求头与所选协议不匹配。');
      if (header.name === 'anthropic-version' && !/^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$/.test(header.value)) throw new Error('anthropic-version 必须使用有效的 YYYY-MM-DD 格式。');
    }
    if (value.protocol === 2 && !value.headers.some(header => header.name === 'anthropic-version')) throw new Error('Anthropic Messages 需要 anthropic-version 请求头。');
    if (aiTimeouts.slice(0, 4).some(([key, , fallback]) => (value[key] || fallback) > (value.total_ms || 300000))) throw new Error('各阶段超时不能超过总超时。');
    if (secret.value) value.secret = serviceText(secret.value, 'API 密钥', 1024);
    return { operation: 'save', profile: value, keep_secret: !!existing && !secret.value };
  });
  syncServiceBusy('ai'); name.focus();
}
function drawSpeechConfig() {
  const data = serviceConfigs.speech.data, root = $('#speech-list');
  const pending = Object.fromEntries([...root.querySelectorAll('select')].filter(input => input.value !== input.dataset.saved).map(input => [input.name, input.value]));
  root.replaceChildren();
  const defaults = el('div', undefined, 'service-defaults'); root.append(defaults);
  for (const [kind, label] of [['asr', '语音转文字（ASR）'], ['tts', '文字转语音（TTS）']]) {
    const row = el('div', undefined, 'service-toolbar'); defaults.append(row);
    const choices = [['none', '不使用'], ...data.providers.filter(provider => provider[kind] && provider.configured && provider.enabled && (kind !== 'tts' || provider.voice)).map(provider => [provider.id, provider.name])];
    if (data[kind] !== 'none' && !choices.some(([id]) => id === data[kind])) choices.push([data[kind], (data.providers.find(provider => provider.id === data[kind])?.name || data[kind]) + '（当前不可用）']);
    const selection = pending['default-' + kind];
    const select = serviceField(row, label, 'default-' + kind, choices.some(([id]) => id === selection) ? selection : data[kind], { choices });
    select.dataset.saved = data[kind];
    row.append(actionButton('设为默认', () => changeService('speech', { operation: 'default', kind, provider: select.value }, label + '默认服务已更新。')));
  }
  for (const provider of data.providers) {
    const row = el('div', undefined, 'service-row'), name = el('div', undefined, 'service-name'), actions = el('div', undefined, 'service-actions');
    name.append(el('strong', provider.name), el('span', (provider.configured ? '已配置' : '未配置') + ' · ' + (provider.enabled ? '已启用' : '已停用') + ' · ' + [provider.asr ? 'ASR' : '', provider.tts ? 'TTS' : ''].filter(Boolean).join(' / '), 'service-help'));
    const remove = actionButton('删除', () => removeService('speech', provider.id, provider.name)); remove.disabled = !provider.configured;
    actions.append(actionButton('配置', () => openSpeechEditor(provider)), remove); row.append(name, actions); root.append(row);
  }
}
async function openSpeechEditor(provider) {
  if (!Object.hasOwn(speechCredentials, provider.id) || !await replaceServiceEditor('speech')) return;
  const editor = serviceForm('speech', '配置 ' + provider.name), form = editor.form; editor.identity = provider.id;
  const fields = el('div', undefined, 'service-fields'); form.append(fields);
  const enabled = serviceField(fields, '启用服务', 'enabled', provider.configured ? provider.enabled : true, { type: 'checkbox' });
  const credentials = speechCredentials[provider.id].map(([key, label, size]) => [key, label, size, serviceField(fields, key === 'workspace' ? label + '（可选）' : label, key, '', { type: 'password', maxLength: size - 1, placeholder: provider.configured ? '留空保留已存凭据' : key === 'workspace' ? '留空使用默认服务' : '请输入' + label })]);
  if (provider.configured) form.append(el('p', '凭据已配置，未填写的凭据将继续保留。', 'service-help'));
  const voice = serviceField(fields, '朗读音色', 'voice', provider.voice || '', { choices: [['', '未设置'], ...provider.voices.map(item => [item.id, item.name])] });
  const minimum = provider.id === 'tencent' ? 0.6 : 0.5;
  const rate = serviceField(fields, '朗读语速（' + minimum + '–2）', 'rate', provider.rate || 1, { type: 'number', min: minimum, max: 2, step: 0.05 });
  const showRate = () => { rate.parentElement.hidden = !voice.value; };
  voice.addEventListener('change', showRate); showRate();
  serviceFormActions('speech', editor, () => {
    const value = { operation: 'save', provider: provider.id, enabled: enabled.checked, voice: voice.value, rate: voice.value ? serviceNumber(rate, '语速', minimum, 2, false) : 0, keep_credentials: !!provider.configured };
    if (value.voice && !provider.voices.some(item => item.id === value.voice)) throw new Error('请选择服务提供的音色。');
    for (const [key, label, size, input] of credentials) {
      if (input.value || !provider.configured) value[key] = serviceText(input.value, label, size, key === 'workspace');
    }
    if (value.app_id && !/^\d+$/.test(value.app_id)) throw new Error('腾讯 AppID 只能包含数字。');
    return value;
  });
  syncServiceBusy('speech'); enabled.focus();
}

function advanced(control) {
  const container = el('div', undefined, 'settings-collection');
  const command = (id, label, value, confirm) => updateSetting({id,label,confirm},value);
  if (control.type === 'networks' || control.type === 'devices') {
    const wifi = control.type === 'networks';
    const bar = el('div',undefined,'collection-bar'), results = el('div', undefined, 'wireless-results');
    results.id = 'results-' + control.id; results.setAttribute('role', 'region'); results.setAttribute('aria-label', wifi ? 'WLAN 搜索结果' : '蓝牙搜索结果');
    const toggle = actionButton('', () => { if (wirelessExpanded.has(control.id)) wirelessExpanded.delete(control.id); else wirelessExpanded.add(control.id); syncResults(); });
    toggle.dataset.local = 'true'; toggle.setAttribute('aria-controls', results.id);
    function syncResults() {
      const expanded = wirelessExpanded.has(control.id);
      results.hidden = !expanded; toggle.textContent = expanded ? '收起列表' : '显示列表'; toggle.setAttribute('aria-expanded', String(expanded));
    }
    bar.append(actionButton('搜索', () => { wirelessExpanded.add(control.id); syncResults(); command(wifi?'wlan_scan':'bt_scan','搜索',true); }), toggle);
    if (wifi) bar.append(actionButton('添加网络',async () => {
      const value=await edit('添加网络',[{id:'ssid',label:'网络名称'},{id:'password',label:'密码',type:'password',required:false}],'连接');
      if(value) command('wlan_connect','连接网络',value);
    }));
    const items = (wifi ? control.networks : control.devices) || [];
    container.append(bar, results); syncResults();
    if (!items.length) results.append(el('p', wifi ? '暂无网络，点击“搜索”查找 WLAN。' : '暂无设备，点击“搜索”查找蓝牙设备。', 'collection-empty'));
    for(const item of items) {
      const row=el('div',undefined,'collection-row'), name=el('div',undefined,'collection-name');
      name.append(el('span',item.name)); if(item.connected) name.append(el('span','已连接','connection-label'));
      else if(wifi ? item.saved : item.paired) name.append(el('span', wifi ? '已保存' : '已配对', 'connection-label'));
      const actions=el('div',undefined,'collection-actions');
      actions.append(actionButton(item.connected?'断开':'连接',async()=>{
        if(wifi) {
          if(item.connected) return command('wlan_disconnect','断开网络',true);
          let value={ssid:item.name};
          if(item.secure && !item.saved) { const password=await edit(item.name,[{id:'password',label:'密码',type:'password'}],'连接'); if(!password)return; value.password=password.password; }
          else if(!item.secure) value.password='';
          command('wlan_connect','连接网络',value);
        } else command(item.connected?'bt_disconnect':'bt_connect',item.name,item.address);
      }));
      if(wifi?item.saved:item.paired) actions.append(actionButton('忘记',()=>command(wifi?'wlan_forget':'bt_forget','忘记设备',wifi?item.name:item.address,'忘记 '+item.name+'？')));
      row.append(name,actions); results.append(row);
    }
    return container;
  }
  if(control.type==='ipv4') {
    const value=control.value;
    container.append(actionButton(value.dhcp?'自动获取':'手动设置',async()=>{
      const mode=await edit('IP 设置',[{id:'dhcp',label:'地址获取',value:value.dhcp?'1':'0',options:[{label:'自动获取',value:'1'},{label:'手动设置',value:'0'}]}]);
      if(!mode)return;
      if(mode.dhcp==='1') return command('ipv4','IP 设置',{dhcp:true});
      const input=await edit('手动设置',[['address','IP 地址'],['netmask','子网掩码'],['gateway','网关'],['dns1','DNS 1'],['dns2','DNS 2']].map(([id,label])=>({id,label,value:value[id]})));
      if(input) command('ipv4','IP 设置',{...input,dhcp:false});
    })); return container;
  }
  if(control.type==='applications') {
    container.append(packageControls());
    for(const app of control.applications) {
      const row=el('div',undefined,'collection-row'); row.append(el('span',app.name),actionButton('卸载',()=>command('app_uninstall','卸载应用',{id:app.id,volume:app.volume},'卸载 '+app.name+'？'))); container.append(row);
    }
    return container;
  }
  return null;
}
function selectSettingsTab(key, focus = false) {
  const selected = settingsNavigation.entries.find(entry => entry.key === key);
  if (!selected) return;
  settingsNavigation.active = key;
  for (const entry of settingsNavigation.entries) {
    const active = entry === selected;
    entry.panel.hidden = !active;
    entry.tab.setAttribute('aria-selected', String(active));
    entry.tab.tabIndex = active ? 0 : -1;
  }
  if (focus) selected.tab.focus();
}
function drawSettingsTabs() {
  const entries = Array.from($('#settings-groups').children).map(panel => ({
    key: panel.dataset.settingsKey, label: panel.dataset.settingsLabel, panel
  }));
  for (const [key, id, label] of [['fonts', 'reading-fonts', '阅读字体'], ['ai', 'ai-settings', '平台 AI'], ['speech', 'speech-settings', '语音服务']])
    entries.push({ key, label, panel: $('#' + id) });
  const rank = key => { const index = settingsOrder.indexOf(key); return index < 0 ? settingsOrder.length : index; };
  entries.sort((a, b) => rank(a.key) - rank(b.key));
  settingsNavigation.entries = entries;
  const tabs = entries.map((entry, index) => {
    const tab = el('button', entry.label, 'settings-tab'); tab.type = 'button'; tab.id = 'settings-tab-' + entry.key;
    tab.setAttribute('role', 'tab'); tab.setAttribute('aria-controls', entry.panel.id);
    entry.panel.setAttribute('role', 'tabpanel'); entry.panel.setAttribute('aria-labelledby', tab.id); entry.panel.tabIndex = 0;
    tab.onclick = () => selectSettingsTab(entry.key);
    tab.onkeydown = event => {
      let next;
      if (event.key === 'ArrowRight') next = (index + 1) % entries.length;
      else if (event.key === 'ArrowLeft') next = (index + entries.length - 1) % entries.length;
      else if (event.key === 'Home') next = 0;
      else if (event.key === 'End') next = entries.length - 1;
      else return;
      event.preventDefault(); selectSettingsTab(entries[next].key, true);
      entries[next].tab.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    };
    entry.tab = tab; return tab;
  });
  $('#settings-tabs').replaceChildren(...tabs);
  selectSettingsTab(entries.some(entry => entry.key === settingsNavigation.active) ? settingsNavigation.active : entries[0].key);
}
async function loadSettings() {
  if (!state.authenticated) return;
  loadFonts();
  loadServiceConfig('ai'); loadServiceConfig('speech');
  const generation = ++state.settings;
  try {
    const result = await api('settings'); if (generation !== state.settings) return;
    serviceApplications = (result.applications || result.groups.flatMap(group => group.items.filter(item => item.type === 'applications').flatMap(item => item.applications || []))).map(app => ({ id: app.id, name: app.name }));
    drawServiceGrants();
    const groups = result.groups.map((group, index) => {
      const section = el('section', undefined, 'settings-group settings-panel');
      section.dataset.settingsKey = group.id || 'group-' + index; section.dataset.settingsLabel = group.label;
      section.id = 'settings-panel-' + section.dataset.settingsKey; section.hidden = true;
      for (const control of group.items) {
        const collection=advanced(control);
        if(collection) { const block=el('div',undefined,'advanced-setting'); block.dataset.type=control.type; if(control.label!==group.label) block.append(el('h3',control.label)); if(control.disabled) collection.querySelectorAll('button:not([data-local]),input,select').forEach(node=>node.disabled=true); block.append(collection); section.append(block); continue; }
        const row = el('div', undefined, 'setting'), label = el('label', control.label), wrap = el('div', undefined, 'setting-control');
        row.dataset.type = control.type;
        const id = 'setting-' + control.id; let input;
        if (control.id === 'wallpaper') {
          input = el('button', '选择设备图片'); input.onclick = () => chooseWallpaper(control); input.disabled = !!control.busy || !!control.disabled;
          wrap.append(el('span', control.busy ? '正在处理墙纸…' : control.custom ? '正在使用自定义墙纸' : '正在使用默认墙纸', 'connection-label'));
        } else if (control.type === 'select') {
          input = document.createElement('select');
          for (const option of control.options) { const item = el('option', option.label); item.value = option.value; input.append(item); }
          input.value = String(control.value); input.onchange = () => updateSetting(control, input.value);
          if (control.id === 'notification_tone' || control.id === 'touch_tone') {
            const preview = actionButton('试听', () => updateSetting(control, input.value));
            preview.disabled = !!control.disabled || Number(input.value) === 0;
            preview.setAttribute('aria-label', '试听' + control.label); wrap.append(input, preview);
          }
        } else if (control.type === 'toggle') {
          input = document.createElement('input'); input.type = 'checkbox'; input.checked = control.value;
          input.onchange = () => updateSetting(control, input.checked);
        } else if (control.type === 'range') {
          input = document.createElement('input'); input.type = 'range'; input.min = control.min ?? 0; input.max = control.max ?? 100; input.step = control.step ?? 1; input.value = control.value;
          const output = el('output', control.value + (control.unit || '')); output.htmlFor = id;
          input.oninput = () => output.textContent = input.value + (control.unit || '');
          input.onchange = () => updateSetting(control, Number(input.value)); wrap.append(input, output);
        } else if (control.type === 'action') {
          input = el('button', control.action || '打开'); input.onclick = () => updateSetting(control, true);
        } else if (control.type === 'text' || control.type === 'datetime') {
          input = document.createElement('input'); input.type = control.type === 'datetime' ? 'datetime-local' : 'text'; input.value = control.value || ''; if (control.maxlength) input.maxLength = control.maxlength;
          const save = el('button', '保存'); save.onclick = () => updateSetting(control, input.value); save.disabled = control.disabled;
          wrap.append(input, save);
        } else { input = el('span', control.value || '—'); }
        input.id = id; input.disabled = !!control.disabled || control.id === 'wallpaper' && !!control.busy; label.htmlFor = id;
        if (!input.parentElement) wrap.append(input); row.append(label, wrap); section.append(row);
      }
      return section;
    });
    $('#settings-groups').replaceChildren(...groups);
    drawSettingsTabs();
    syncSettingsBusy();
  } catch (error) { if (generation === state.settings) { if (!settingsNavigation.entries.length) drawSettingsTabs(); report(error); } }
}
$('#refresh-settings').onclick = loadSettings;
function navigate() {
  if (!state.authenticated) return;
  const page = ['files', 'device', 'settings'].includes(location.hash.slice(1)) ? location.hash.slice(1) : 'files';
  document.querySelectorAll('.page').forEach(node => node.hidden = node.id !== page);
  document.querySelectorAll('[data-page]').forEach(node => { if (node.dataset.page === page) node.setAttribute('aria-current', 'page'); else node.removeAttribute('aria-current'); });
  document.title = { files: '我的文件', device: '设备', settings: '设置' }[page] + ' · 墨水屏';
  ({ files: loadFiles, device: loadInfo, settings: loadSettings })[page]();
}
window.addEventListener('hashchange', navigate);
function resetSecrets() {
  for (const id of ['password', 'password-confirm', 'setup-code']) $('#' + id).value = '';
  document.querySelectorAll('.password-toggle').forEach(button => {
    $('#' + button.dataset.password).type = 'password'; button.textContent = '显示';
    button.setAttribute('aria-pressed', 'false'); button.setAttribute('aria-label', button.dataset.password === 'password' ? '显示密码' : '显示确认密码');
  });
  document.querySelectorAll('#login input').forEach(input => input.removeAttribute('aria-invalid'));
}
function authError(message = '') { $('#login-error').textContent = message; $('#login-error').hidden = !message; }
function syncAuthControls() {
  const setup = auth.mode === 'setup-ready', waiting = Date.now() < auth.retryUntil;
  const disabled = auth.busy || waiting;
  $('#sign-in').dataset.state = auth.busy ? 'busy' : waiting ? 'rate-limit' : auth.mode;
  $('#login').setAttribute('aria-busy', String(auth.busy));
  document.querySelectorAll('#login input, #login button').forEach(input => { input.disabled = disabled || (!setup && !!input.closest('.setup-field')); });
  $('#auth-submit').textContent = auth.busy ? (setup ? '正在创建账号…' : '正在登录…') : setup ? '创建账号并登录' : '登录';
  $('#auth-refresh').disabled = disabled;
  const progress = auth.busy ? (auth.mode === 'checking' ? '正在检查设备状态…' : auth.logoutToken ? '正在退出登录…' : setup ? '正在创建账号…' : '正在登录…') : waiting ? '请等待 ' + Math.ceil((auth.retryUntil - Date.now()) / 1000) + ' 秒后重试。' : '';
  if ($('#auth-progress').textContent !== progress) $('#auth-progress').textContent = progress;
  $('#auth-progress').hidden = !progress;
}
function showAuth(mode, message = '', focus = false) {
  const changed = auth.mode !== mode;
  auth.mode = mode; auth.busy = mode === 'checking';
  if (changed) resetSecrets();
  const setup = mode === 'setup-ready';
  const text = {
    checking: ['连接设备', '正在检查登录状态', '请稍候，正在连接设备。'],
    'setup-locked': ['首次使用 · 1 / 2', '请先在设备上开启设置', '设备尚未创建账号。请拿起墨水屏，开启首次账号设置。'],
    'setup-ready': ['首次使用 · 2 / 2', '创建首个账号', '请使用设备显示的验证码，在设置窗口关闭前完成创建。'],
    login: ['欢迎回来', '登录墨水屏', ''],
    error: ['连接设备', '暂时无法连接', '请确认浏览器已连接设备热点，或与设备接入同一 Wi-Fi。']
  }[mode];
  $('#auth-step').textContent = text[0]; $('#auth-title').textContent = text[1]; $('#auth-description').textContent = text[2];
  $('#auth-description').hidden = !text[2];
  $('#sign-in').hidden = false; $('.workspace').hidden = true;
  $('#setup-locked').hidden = mode !== 'setup-locked'; $('#login').hidden = !setup && mode !== 'login';
  document.querySelectorAll('.setup-field').forEach(node => node.hidden = !setup);
  $('#username-hint').hidden = $('#password-hint').hidden = !setup;
  $('#password').autocomplete = setup ? 'new-password' : 'current-password';
  $('#password-confirm').required = $('#setup-code').required = setup;
  $('#auth-refresh').hidden = mode === 'checking';
  $('#auth-refresh').textContent = auth.logoutToken ? '重试退出登录' : mode === 'setup-locked' ? '我已在设备上开启，重新检查' : mode === 'error' ? '重新连接' : '刷新登录状态';
  authError(message); syncAuthControls();
  if (focus) (setup || mode === 'login' ? $('#username') : $('#auth-title')).focus();
}
function rateLimit(error) {
  const seconds = Number.isFinite(error.retryAfter) && error.retryAfter > 0 ? Math.min(error.retryAfter, 3600) : 30;
  auth.retryUntil = Date.now() + seconds * 1000;
  clearInterval(auth.retryTimer);
  auth.retryTimer = setInterval(() => {
    if (Date.now() >= auth.retryUntil) { clearInterval(auth.retryTimer); auth.retryTimer = null; auth.retryUntil = 0; authError('现在可以重试。'); }
    syncAuthControls();
  }, 250);
}
function connected(session) {
  const csrf = session?.csrf;
  if (session?.authenticated === false || typeof csrf !== 'string' || !csrf || typeof session?.username !== 'string' || !session.username) throw new Error('设备未返回完整的登录信息，请重新登录。');
  clearInterval(auth.retryTimer); auth.retryTimer = null; auth.retryUntil = 0;
  state.csrf = csrf; state.username = session.username; state.authenticated = true;
  auth.busy = false; resetSecrets(); $('#username').value = ''; authError();
  $('#session-username').textContent = session.username;
  $('#sign-in').hidden = true; $('.workspace').hidden = false; $('.workspace').inert = false;
  navigate(); if (location.hash !== '#device') loadInfo();
  $('.page:not([hidden]) h1').setAttribute('tabindex', '-1'); $('.page:not([hidden]) h1').focus();
}
async function checkAuth(focus = false, message = '') {
  if (auth.busy || Date.now() < auth.retryUntil) return;
  if (auth.logoutToken) return logout();
  const epoch = state.epoch;
  showAuth('checking');
  try {
    const status = await api('auth/status', {}, true);
    if (typeof status.configured !== 'boolean' || typeof status.setup_open !== 'boolean' || typeof status.authenticated !== 'boolean') throw new Error('设备返回了无效的登录状态，请重试。');
    if (status.authenticated) {
      try { connected(await api('session', {}, true)); return; }
      catch (error) { if (error.status !== 401) throw error; }
    }
    showAuth(status.configured ? 'login' : status.setup_open ? 'setup-ready' : 'setup-locked', message, focus);
  } catch (error) {
    if (epoch !== state.epoch) return;
    showAuth('error', error.message, focus);
    if (error.status === 429) { rateLimit(error); syncAuthControls(); }
  }
}
function validateLogin(setup) {
  const username = $('#username'), password = $('#password'), confirm = $('#password-confirm'), code = $('#setup-code');
  document.querySelectorAll('#login input').forEach(input => input.removeAttribute('aria-invalid'));
  let input, message;
  if (!/^[A-Za-z0-9_.-]{3,32}$/.test(username.value)) { input = username; message = '用户名需要 3–32 个字符，只能包含英文字母、数字、下划线、点和短横线。'; }
  else if (new TextEncoder().encode(password.value).length < 8 || new TextEncoder().encode(password.value).length > 64) { input = password; message = '密码需要 8–64 字节，请检查密码长度。'; }
  else if (/[\u0000-\u001f\u007f]/.test(password.value) || Array.from(password.value).some(char => char.codePointAt(0) >= 0xd800 && char.codePointAt(0) <= 0xdfff)) { input = password; message = '密码不能包含控制字符或无效字符，请重新输入。'; }
  else if (setup && password.value !== confirm.value) { input = confirm; message = '两次输入的密码不一致，请重新确认。'; }
  else if (setup && !code.value.trim()) { input = code; message = '请输入设备显示的一次性验证码。'; }
  if (!input) return true;
  input.setAttribute('aria-invalid', 'true'); authError(message); $('#sign-in').dataset.state = 'validation'; input.focus(); return false;
}
$('#login').onsubmit = async event => {
  event.preventDefault();
  if (auth.busy || Date.now() < auth.retryUntil || !['login', 'setup-ready'].includes(auth.mode)) return;
  const setup = auth.mode === 'setup-ready';
  if (!validateLogin(setup)) return;
  const epoch = state.epoch, value = { username: $('#username').value, password: $('#password').value };
  if (setup) value.code = $('#setup-code').value.trim();
  auth.busy = true; authError(); syncAuthControls();
  try {
    const session = await api(setup ? 'auth/setup' : 'session', json('POST', value), true);
    connected(session);
  } catch (error) {
    if (epoch !== state.epoch) return;
    auth.busy = false;
    if (setup && (error.code === 'setup_closed' || [403, 409, 410].includes(error.status) && error.code !== 'busy')) {
      resetSecrets(); await checkAuth(true, '首次设置状态已变更。若设置窗口已关闭，请在设备上重新开启并使用新的验证码。');
    } else {
      const message = error.status === 401 ? (setup ? '验证码无效或已过期，请核对设备上的验证码。' : '用户名或密码不正确，请重新输入。') : error.status === 400 ? '输入内容未通过设备校验，请检查用户名、密码和验证码。' : error.status === 429 ? '尝试次数过多，请稍后重试。' : error.message;
      authError(message);
      if (error.status === 429) rateLimit(error);
      if (!setup && error.status === 401) { resetSecrets(); syncAuthControls(); $('#password').focus(); }
    }
  } finally { if (epoch === state.epoch) { auth.busy = false; syncAuthControls(); } }
};
document.querySelectorAll('.password-toggle').forEach(button => button.onclick = () => {
  const input = $('#' + button.dataset.password), visible = input.type === 'password';
  input.type = visible ? 'text' : 'password'; button.textContent = visible ? '隐藏' : '显示'; button.setAttribute('aria-pressed', String(visible));
  button.setAttribute('aria-label', (visible ? '隐藏' : '显示') + (input.id === 'password-confirm' ? '确认密码' : '密码'));
});
document.querySelectorAll('#login input').forEach(input => input.addEventListener('input', () => { input.removeAttribute('aria-invalid'); authError(); syncAuthControls(); }));
async function logout() {
  if (auth.busy) return;
  const csrf = state.csrf || auth.logoutToken;
  if (!csrf) return;
  try { sessionStorage.removeItem(packageStorageKey); } catch (_) {}
  clearSession(); auth.logoutToken = csrf;
  showAuth('login'); auth.busy = true; syncAuthControls(); $('#auth-title').focus();
  const epoch = state.epoch;
  try {
    await api('logout', { method: 'POST', headers: { 'X-CSRF-Token': csrf } }, true);
    auth.logoutToken = ''; showAuth('login', '', true); $('#auth-progress').textContent = '已退出登录。'; $('#auth-progress').hidden = false;
  } catch (error) {
    if (epoch !== state.epoch) return;
    if (error.status === 401) { auth.logoutToken = ''; showAuth('login', '', true); $('#auth-progress').textContent = '已退出登录。'; $('#auth-progress').hidden = false; }
    else { showAuth('error', '页面数据已清除，但设备尚未确认退出。请重新连接设备并重试退出。', true); if (error.status === 429) { rateLimit(error); syncAuthControls(); } }
  }
}
$('#logout').onclick = logout;
$('#auth-refresh').onclick = () => checkAuth(true);
window.addEventListener('pagehide', () => clearSession());
window.addEventListener('pageshow', event => { if (event.persisted) checkAuth(); });
if (location.hash && !['#files', '#device', '#settings'].includes(location.hash)) history.replaceState(null, '', location.pathname + location.search + '#files');
checkAuth();
