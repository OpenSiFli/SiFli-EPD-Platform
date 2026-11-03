import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const docsRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const repositoryRoot = path.dirname(docsRoot);
export const generatedRoot = path.join(docsRoot, '.generated');
export const repository = 'https://github.com/OpenSiFli/SiFli-EPD-Platform';
export const defaultSiteUrl = 'https://docs.sifli.com/projects/epd-platform/';
export const posix = (value) => value.split(path.sep).join('/');
export const readJson = (name) => JSON.parse(fs.readFileSync(path.join(docsRoot, 'data', name), 'utf8'));

export function location(value = defaultSiteUrl) {
  const url = new URL(value);
  if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash || /\/\/|%2f|%5c/i.test(url.pathname)) {
    throw new Error('DOCS_SITE_URL must be an HTTPS URL without credentials, query, fragment or ambiguous path');
  }
  const base = url.pathname.replace(/\/*$/, '/');
  return { origin: url.origin, base, url: url.origin + base };
}

export function urlFor(id, section) {
  const parts = id.replace(/\.md$/, '').split('/');
  if (parts.at(-1) === 'README') parts.pop();
  if (parts[0] === section || ['tutorials', 'device'].includes(section)) parts.shift();
  return `/${section}/${parts.join('/')}/`;
}

export function listFiles(directory) {
  if (!fs.existsSync(directory)) return [];
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const filename = path.join(directory, entry.name);
    return entry.isDirectory() ? listFiles(filename) : [filename];
  });
}

export function loadCatalog() {
  const { groups, sections } = readJson('catalog.json');
  const components = readJson('components.json');
  const examples = readJson('examples.json');
  const seen = new Set();
  const urls = new Set();
  const articles = groups.flatMap((group) => group.documents.map(([id, title]) => {
    if (seen.has(id)) throw new Error(`Duplicate article: ${id}`);
    seen.add(id);
    const component = components.find((entry) => entry.page === id);
    const source = component ? component.source : `docs/content/${id}`;
    const absolute = path.resolve(repositoryRoot, source);
    if (!absolute.startsWith(repositoryRoot + path.sep) || !fs.statSync(absolute).isFile()) throw new Error(`Missing source: ${source}`);
    const url = urlFor(id, group.section);
    if (urls.has(url)) throw new Error(`Duplicate route: ${url}`);
    urls.add(url);
    return { id, title, source, url, section: group.section, group: group.id, groupTitle: group.title, component: !!component };
  }));
  const listed = new Set(articles.filter((entry) => !entry.component).map((entry) => path.resolve(repositoryRoot, entry.source)));
  const unlisted = listFiles(path.join(docsRoot, 'content')).filter((file) => file.endsWith('.md') && !listed.has(file));
  if (unlisted.length) throw new Error(`Unlisted articles: ${unlisted.join(', ')}`);
  for (const component of components) if (!seen.has(component.page)) throw new Error(`Unlisted component: ${component.source}`);
  for (const group of groups) if (!sections.some((section) => section.id === group.section && section.groups.includes(group.id))) throw new Error(`Unlisted group: ${group.id}`);
  return { articles, groups, sections, components, examples };
}

export function sourceMap(catalog) {
  const result = new Map(catalog.articles.map((entry) => [path.resolve(repositoryRoot, entry.source), entry.url]));
  const publicRoot = path.join(docsRoot, 'public');
  for (const file of listFiles(publicRoot)) result.set(file, '/' + posix(path.relative(publicRoot, file)));
  for (const component of catalog.components) {
    const directory = path.join(repositoryRoot, path.dirname(component.source), 'assets');
    const route = catalog.articles.find((entry) => entry.source === component.source).url;
    for (const file of listFiles(directory)) result.set(file, route + 'assets/' + posix(path.relative(directory, file)));
  }
  return result;
}

export function resolveLink(href, source, sources) {
  if (!href || /^(?:[a-z][a-z\d+.-]*:|\/\/|#)/i.test(href) || href.startsWith('/')) return href;
  const split = href.search(/[?#]/);
  const pathname = split < 0 ? href : href.slice(0, split);
  const suffix = split < 0 ? '' : href.slice(split);
  const absolute = path.resolve(repositoryRoot, path.dirname(source), decodeURIComponent(pathname));
  if (sources.has(absolute)) return sources.get(absolute) + suffix;
  if (absolute.startsWith(repositoryRoot + path.sep) && fs.existsSync(absolute)) {
    const kind = fs.statSync(absolute).isDirectory() ? 'tree' : 'blob';
    return `${repository}/${kind}/main/${posix(path.relative(repositoryRoot, absolute))}${suffix}`;
  }
  throw new Error(`${source}: missing link ${href}`);
}

export function slugify(text) {
  return text.trim().toLowerCase().replace(/[^\p{L}\p{N}\p{M}_\-\s]/gu, '').replace(/\s/g, '-') || 'section';
}
