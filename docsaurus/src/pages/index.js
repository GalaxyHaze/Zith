import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import Heading from '@theme/Heading';
import styles from './index.module.css';

export default function Home() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout title="Home" description={siteConfig.tagline}>
      <main className={styles.main}>
        <div className={styles.hero}>
          <Heading as="h1" className={styles.title}>
            {siteConfig.title}
          </Heading>
          <p className={styles.tagline}>{siteConfig.tagline}</p>
          <div className={styles.buttons}>
            <Link className="button button--primary button--lg" to="/docs/intro/overview">
              Get Started
            </Link>
            <Link className="button button--secondary button--lg" to="https://github.com/galaxyhaze/Zith">
              GitHub
            </Link>
          </div>
        </div>
      </main>
    </Layout>
  );
}