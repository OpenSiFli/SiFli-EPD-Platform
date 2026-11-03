import DefaultTheme from 'vitepress/theme';
import Layout from './Layout.vue';
import DocumentationHome from './DocumentationHome.vue';
import './custom.css';

export default {
  extends: DefaultTheme,
  Layout,
  enhanceApp({ app }) { app.component('DocumentationHome', DocumentationHome); }
};
