// @ts-check

/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  tutorialSidebar: [
    {
      type: 'category',
      label: 'Getting Started',
      link: {
        type: 'doc',
        id: 'intro/overview',
      },
      items: [
        'intro/overview',
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
        id: 'technical/language/overview',
      },
      items: [
        'technical/language/overview',
        'technical/language/spec-core-topics',
      ],
    },
    {
      type: 'category',
      label: 'CLI Reference',
      link: {
        type: 'doc',
        id: 'technical/cli/overview',
      },
      items: [
        'technical/cli/overview',
        'technical/cli/check-command',
      ],
    },
    {
      type: 'category',
      label: 'Project Configuration',
      link: {
        type: 'doc',
        id: 'technical/project/overview',
      },
      items: [
        'technical/project/overview',
      ],
    },
    {
      type: 'category',
      label: 'FAQ',
      link: {
        type: 'doc',
        id: 'technical/faq/overview',
      },
      items: [
        'technical/faq/overview',
        'technical/faq/philosophy',
        'technical/faq/security',
        'technical/faq/rust-comparison',
        'technical/faq/use-cases',
      ],
    },
  ],
};

module.exports = sidebars;