// @ts-check

/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  tutorialSidebar: [
    {
      type: 'category',
      label: 'Getting Started',
      link: {
        type: 'doc',
        id: 'intro/01-overview',
      },
      items: [
        'intro/01-overview',
        'intro/why-zith',
        'intro/installation',
        'intro/quickstart',
      ],
    },
    {
      type: 'category',
      label: 'Language Guide',
      link: {
        type: 'doc',
        id: 'technical/language/01-overview',
      },
      items: [
        'technical/language/01-overview',
      ],
    },
    {
      type: 'category',
      label: 'CLI Reference',
      link: {
        type: 'doc',
        id: 'technical/cli/01-overview',
      },
      items: [
        'technical/cli/01-overview',
        'technical/cli/02-overview',
      ],
    },
    {
      type: 'category',
      label: 'Project Configuration',
      link: {
        type: 'doc',
        id: 'technical/project/01-overview',
      },
      items: [
        'technical/project/01-overview',
      ],
    },
    {
      type: 'category',
      label: 'FAQ',
      link: {
        type: 'doc',
        id: 'technical/faq/01-overview',
      },
      items: [
        'technical/faq/01-overview',
        'technical/faq/02-philosophy',
        'technical/faq/03-security',
        'technical/faq/04-rust-comparison',
        'technical/faq/05-use-cases',
      ],
    },
    {
      type: 'category',
      label: 'Raw & Unsafe',
      link: {
        type: 'doc',
        id: 'technical/raw/01-overview',
      },
      items: [
        'technical/raw/01-overview',
      ],
    },
  ],
};

module.exports = sidebars;
