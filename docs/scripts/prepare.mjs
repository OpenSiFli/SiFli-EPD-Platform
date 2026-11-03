import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { stringify } from 'yaml';
import { docsRoot, repositoryRoot, generatedRoot, repository, loadCatalog, listFiles, location } from './catalog.mjs';

const frontmatter = (data, body) => `---\n${stringify(data)}---\n\n${body.trim()}\n`;
export const stripFrontmatter = (body) => body.replace(/^---\r?\n[\s\S]*?\r?\n---(?:\r?\n|$)/, '');

export function prepare() {
  const catalog = loadCatalog();
  if (path.dirname(generatedRoot) !== docsRoot || path.basename(generatedRoot) !== '.generated') throw new Error('Invalid generated directory');
  fs.rmSync(generatedRoot, { recursive: true, force: true });
  fs.mkdirSync(generatedRoot, { recursive: true });
  const outputs = new Set();
  const writePage = (url, body) => {
    const target = path.join(generatedRoot, url, 'index.md');
    if (outputs.has(target)) throw new Error(`Output collision: ${url}`);
    outputs.add(target);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, body);
  };
  for (const article of catalog.articles) {
    const siblings = catalog.articles.filter((entry) => entry.group === article.group);
    const position = siblings.indexOf(article);
    const neighbor = (entry) => entry ? { text: entry.title, link: entry.url } : false;
    const original = stripFrontmatter(fs.readFileSync(path.join(repositoryRoot, article.source), 'utf8'));
    const body = original.replace(/^#\s+[^\n]+/m, `# ${article.title}`);
    writePage(article.url, frontmatter({ title: article.title, sourcePath: article.source, section: article.section,
      groupTitle: article.groupTitle, prev: neighbor(siblings[position - 1]), next: neighbor(siblings[position + 1]) }, body));
  }
  const groupList = (group) => catalog.articles.filter((entry) => entry.group === group.id).map((entry) => `- [${entry.title}](${entry.url})`).join('\n');
  for (const section of catalog.sections) {
    const groups = section.groups.map((id) => catalog.groups.find((group) => group.id === id));
    const parts = [`# ${section.title}`];
    for (const group of groups) {
      if (groups.length > 1) parts.push(`## ${group.title}`);
      parts.push(groupList(group));
      if (groups.length > 1) writePage(`/${section.id}/${group.id}/`, frontmatter({ title: group.title, section: section.id, prev: false, next: false }, `# ${group.title}\n\n${groupList(group)}`));
    }
    if (section.id === 'examples') {
      parts.push('| 应用 | 说明 | 源码 |', '| --- | --- | --- |');
      for (const example of catalog.examples) {
        if (!fs.existsSync(path.join(repositoryRoot, example.source))) throw new Error(`Missing example: ${example.source}`);
        parts.push(`| ${example.title} | ${example.description} | [${example.id}](${repository}/tree/main/${example.source}) |`);
      }
    }
    writePage(`/${section.id}/`, frontmatter({ title: section.title, section: section.id, prev: false, next: false }, parts.join('\n\n').replace(/\|\n\n\|/g, '|\n|')));
  }
  fs.copyFileSync(path.join(docsRoot, 'index.md'), path.join(generatedRoot, 'index.md'));
  const publicRoot = path.join(generatedRoot, 'public');
  fs.cpSync(path.join(docsRoot, 'public'), publicRoot, { recursive: true });
  for (const component of catalog.components) {
    const from = path.join(repositoryRoot, path.dirname(component.source), 'assets');
    if (!listFiles(from).length) continue;
    const article = catalog.articles.find((entry) => entry.source === component.source);
    fs.cpSync(from, path.join(publicRoot, article.url, 'assets'), { recursive: true });
  }
  const target = location(process.env.DOCS_SITE_URL);
  fs.writeFileSync(path.join(publicRoot, 'build-manifest.json'), JSON.stringify({ base: target.base, articles: catalog.articles, pages: outputs.size + 2, components: catalog.components.map((entry) => entry.source) }, null, 2) + '\n');
  console.log(`Prepared ${catalog.articles.length} articles including ${catalog.components.length} component READMEs.`);
  return catalog;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) prepare();
