<script setup>
import { computed, ref, onMounted, onUnmounted } from 'vue';
import { useData, withBase } from 'vitepress';
import DefaultTheme from 'vitepress/theme';

const { Layout } = DefaultTheme;
const { frontmatter, theme } = useData();
const section = computed(() => theme.value.sidebar.find((entry) => entry.link === `/${frontmatter.value.section}/`));
const repository = 'https://github.com/OpenSiFli/SiFli-EPD-Platform';
const sourceUrl = computed(() => frontmatter.value.sourcePath ? `${repository}/blob/main/${frontmatter.value.sourcePath}` : null);
const showTop = ref(false);
const onScroll = () => { showTop.value = window.scrollY > 500; };
onMounted(() => { onScroll(); window.addEventListener('scroll', onScroll, { passive: true }); });
onUnmounted(() => window.removeEventListener('scroll', onScroll));
const scrollTop = () => { window.scrollTo({ top: 0, behavior: 'auto' }); };
</script>

<template>
  <Layout>
    <template #nav-bar-title-after><span class="documentation-version">v0.5 仅供内部参考</span></template>
    <template #sidebar-nav-before>
      <a class="sidebar-home" :href="withBase('/')" :aria-current="frontmatter.pageClass === 'documentation-home' ? 'page' : undefined">文档首页</a>
    </template>
    <template #doc-before>
      <nav v-if="section" class="document-breadcrumb" aria-label="当前位置">
        <a :href="withBase('/')">文档首页</a><span>/</span>
        <a :href="withBase(section.link)">{{ section.text }}</a>
        <template v-if="frontmatter.sourcePath"><span>/</span><span>{{ frontmatter.title }}</span></template>
      </nav>
    </template>
    <template #aside-top>
      <div class="documentation-repository">
        <a :href="repository" target="_blank" rel="noreferrer">
          <svg class="repository-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><path d="M5 3h14v18H6a3 3 0 0 1 0-6h13M5 3v12M8 7h7M8 10h5" /></svg>
          <span><small>OpenSiFli</small><strong>SiFli-EPD-Platform</strong></span>
          <span aria-hidden="true">↗</span>
        </a>
      </div>
      <a v-if="sourceUrl" class="documentation-source" :href="sourceUrl" target="_blank" rel="noreferrer">查看本页源码 <span aria-hidden="true">↗</span></a>
    </template>
    <template #layout-bottom>
      <button v-show="showTop" class="back-to-top" aria-label="返回顶部" @click="scrollTop">↑</button>
    </template>
  </Layout>
</template>
