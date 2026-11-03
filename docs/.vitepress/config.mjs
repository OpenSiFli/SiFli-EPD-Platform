import { defineConfig } from 'vitepress';
import path from 'node:path';
import { docsRoot, loadCatalog, repository, location, sourceMap, resolveLink, slugify } from '../scripts/catalog.mjs';
import { tokenize } from './search.mjs';

const catalog = loadCatalog();
const sources = sourceMap(catalog);
const site = location(process.env.DOCS_SITE_URL);
const articleFiles = new Map(catalog.articles.map((entry) => [entry.url.slice(1) + 'index.md', entry]));
const sidebar = catalog.sections.map((section) => ({
  text: section.title, link: `/${section.id}/`, collapsed: true,
  items: section.groups.flatMap((id) => {
    const group = catalog.groups.find((entry) => entry.id === id);
    const items = catalog.articles.filter((entry) => entry.group === id).map((entry) => ({ text: entry.title, link: entry.url }));
    return section.groups.length === 1 ? items : [{ text: group.title, collapsed: true, items }];
  })
}));

export default defineConfig({
  lang: 'zh-CN',
  title: 'SiFli EPD Platform',
  description: '平台开发文档',
  srcDir: '.generated',
  base: site.base,
  cleanUrls: false,
  sitemap: { hostname: site.origin, transformItems: (items) => items
    .filter((entry) => !entry.url.endsWith('404.html'))
    .map((entry) => ({ ...entry, url: site.base + entry.url.replace(/^\//, '') })) },
  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: site.base + 'assets/logo.svg' }]],
  themeConfig: {
    logo: '/assets/logo.svg',
    siteTitle: 'SiFli EPD Platform',
    nav: [
      { text: '开始使用', link: '/tutorials/' },
      { text: '开发指南', link: '/guides/' },
      { text: '系统架构', link: '/architecture/' },
      { text: '更多', items: catalog.sections.filter((entry) => !['tutorials', 'guides', 'architecture'].includes(entry.id)).map((entry) => ({ text: entry.title, link: `/${entry.id}/` })) }
    ],
    sidebar,
    socialLinks: [{ icon: 'github', link: repository }],
    outline: { level: [2, 3], label: '本页目录' },
    docFooter: { prev: '上一篇', next: '下一篇' },
    notFound: { title: '页面不存在', quote: '请从文档导航中选择页面，或使用搜索查找内容。', linkText: '返回文档首页', linkLabel: '返回文档首页' },
    editLink: { text: '查看源码', pattern: (page) => {
      const root = 'https://github.com/OpenSiFli/SiFli-EPD-Platform';
      return page.frontmatter.sourcePath ? `${root}/blob/main/${page.frontmatter.sourcePath}` : `${root}/tree/main/docs`;
    } },
    footer: { message: 'SiFli EPD Platform', copyright: 'v0.5 仅供内部参考' },
    search: { provider: 'local', options: {
      miniSearch: { options: { tokenize }, searchOptions: { combineWith: 'AND', prefix: true, fuzzy: false } },
      translations: {
        button: { buttonText: '搜索文档', buttonAriaLabel: '搜索文档' },
        modal: { displayDetails: '显示详细结果', resetButtonTitle: '清除搜索', backButtonTitle: '关闭搜索', noResultsText: '没有找到相关文档', footer: { selectText: '选择', navigateText: '切换', closeText: '关闭' } }
      }
    } },
    returnToTopLabel: '返回顶部', sidebarMenuLabel: '文档目录', darkModeSwitchLabel: '外观',
    lightModeSwitchTitle: '切换浅色模式', darkModeSwitchTitle: '切换深色模式', skipToContentLabel: '跳至正文'
  },
  markdown: {
    codeCopyButtonTitle: '复制代码',
    anchor: { slugify },
    config(md) {
      md.core.ruler.after('inline', 'platform-source-links', (state) => {
        const file = state.env.relativePath || (state.env.path && path.relative(path.join(docsRoot, '.generated'), state.env.path).split(path.sep).join('/'));
        const article = articleFiles.get(file);
        if (!article) return;
        const visit = (tokens) => {
          for (const token of tokens) {
            const attribute = token.type === 'link_open' ? 'href' : token.type === 'image' ? 'src' : null;
            if (attribute) token.attrSet(attribute, resolveLink(token.attrGet(attribute), article.source, sources));
            if (token.children) visit(token.children);
          }
        };
        visit(state.tokens);
      });
    }
  }
});
