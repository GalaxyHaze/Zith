// @ts-check
// Note: // @ts-check not needed if you don't want type checking
const {themes} = require('@docusaurus/core/lib/client/exports');

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Zith Programming Language',
  tagline: 'A modern systems programming language with clarity and safety',
  favicon: 'img/favicon.ico',

  // Set the production url of your site here
  url: 'https://zith-lang.dev',
  // Set the /<baseUrl>/ pathname under which your site is served
  baseUrl: '/',

  // GitHub pages deployment config.
  organizationName: 'galaxyhaze',
  projectName: 'Zith',
  trailingSlash: false,

  // See https://docusaurus.io/docs/api/docusaurus-config#deploymentconfig
  deploymentBranch: 'gh-pages',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          sidebarPath: require.resolve('./sidebars.js'),
          routeBasePath: '/docs',
          editUrl: 'https://github.com/galaxyhaze/Zith/tree/main/docusaurus-docs/',
        },
        blog: false,
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      navbar: {
        title: 'Zith',
        logo: {
          alt: 'Zith Logo',
          src: 'img/logo.svg',
        },
        items: [
          {
            type: 'docSidebar',
            sidebarId: 'tutorialSidebar',
            position: 'left',
            label: 'Documentation',
          },
          {
            href: 'https://github.com/galaxyhaze/Zith',
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              {
                label: 'Getting Started',
                to: '/docs/intro/01-overview',
              },
              {
                label: 'Language Guide',
                to: '/docs/technical/language/01-overview',
              },
              {
                label: 'CLI Reference',
                to: '/docs/technical/cli/01-overview',
              },
            ],
          },
          {
            title: 'Community',
            items: [
              {
                label: 'GitHub Discussions',
                href: 'https://github.com/galaxyhaze/Zith/discussions',
              },
            ],
          },
          {
            title: 'More',
            items: [
              {
                label: 'GitHub',
                href: 'https://github.com/galaxyhaze/Zith',
              },
            ],
          },
        ],
        copyright: `Copyright © ${new Date().getFullYear()} Zith Programming Language.`,
      },
      prism: {
        theme: themes.oneLight,
        darkTheme: themes.oneDark,
        additionalLanguages: ['rust', 'bash', 'toml'],
      },
      colorMode: {
        defaultMode: 'dark',
        disableSwitch: true,
        respectPrefersColorScheme: false,
      },
    }),
};

module.exports = config;
