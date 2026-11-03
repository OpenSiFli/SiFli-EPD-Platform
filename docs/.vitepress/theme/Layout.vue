<script setup>
import { computed, ref, onMounted, onUnmounted } from 'vue';
import { useData, withBase } from 'vitepress';
import DefaultTheme from 'vitepress/theme';

const { Layout } = DefaultTheme;
const { frontmatter, theme } = useData();
const section = computed(() => theme.value.sidebar.find((entry) => entry.link === `/${frontmatter.value.section}/`));
const showTop = ref(false);
const onScroll = () => { showTop.value = window.scrollY > 500; };
onMounted(() => { onScroll(); window.addEventListener('scroll', onScroll, { passive: true }); });
onUnmounted(() => window.removeEventListener('scroll', onScroll));
const scrollTop = () => { window.scrollTo({ top: 0, behavior: 'auto' }); };
</script>

<template>
  <Layout>
    <template #nav-bar-content-after><span class="documentation-version">v0.5 仅供内部参考</span></template>
    <template #doc-before>
      <nav v-if="section" class="document-breadcrumb" aria-label="当前位置">
        <a :href="withBase('/')">文档首页</a><span>/</span>
        <a :href="withBase(section.link)">{{ section.text }}</a>
        <template v-if="frontmatter.sourcePath"><span>/</span><span>{{ frontmatter.title }}</span></template>
      </nav>
    </template>
    <template #layout-bottom>
      <button v-show="showTop" class="back-to-top" aria-label="返回顶部" @click="scrollTop">↑</button>
    </template>
  </Layout>
</template>
