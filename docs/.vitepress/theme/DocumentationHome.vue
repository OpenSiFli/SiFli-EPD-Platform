<script setup>
import { useData, withBase } from 'vitepress';

const { frontmatter } = useData();
</script>

<template>
  <section class="documentation-intro" aria-labelledby="documentation-title">
    <img class="documentation-mark" :src="withBase('/assets/logo.svg')" alt="" width="48" height="48" />
    <h1 id="documentation-title">{{ frontmatter.hero.name }}</h1>
    <p>{{ frontmatter.hero.tagline }}</p>
    <nav class="documentation-actions" aria-label="快速开始">
      <a v-for="action in frontmatter.hero.actions" :key="action.link"
        :class="['documentation-action', action.theme]" :href="withBase(action.link)">{{ action.text }}</a>
    </nav>
  </section>
</template>
