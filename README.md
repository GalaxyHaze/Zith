#  Zith — Official Website

> This branch hosts the official website for the **Zith** programming language.

---

##  Creator

The site was created by [MieldewMeat](https://github.com/MieldewMeat).

---

##  Documentation

Documentation for the Zith programming language is currently **in progress** and will be published here as it grows.

### Documentation build

Editorial pages are Markdown-first under `docs/pages`. The Language Reference is synchronized from
the adjacent compiler checkout at `../Zith/docs`; generated HTML fragments remain versioned for
static hosting.

```bash
make docs
make docs-check
```

`make docs-check` fails if generated fragments, navigation, or the specification manifest are out
of date.

---

##  Contribute

We welcome community contributions! If you'd like to support the project by proposing documentation:

1. **Reach out** via [Discord](#https://discord.com/invite/a7h4cpWHg4) to coordinate with the team ( note: serve is not configured yet, We are sorry! )
2. **Follow** the project's contribution rules and guidelines
3. **Submit** your proposal for review

>  The team will evaluate all submissions before adding them to the site.

---

## ️ Tech Stack

- **HTML only** — no frameworks, no preprocessors
- **Example**:
```html
    <h1>Title</h1>
    <br>
    <h2>Overview</h2>
    <br>
    <p>Text example.</p>
    <br>
    <h2>See also</h2>
    <br>
    <p>To jump across files</p>
    <a href="./D-build.html">Build Guide</a>
    <br>
    <p>Or jump into a section</p>
    <a href="./D-build.html#building">Building Section</a>
  ```
- The site handles all logic natively on its own

---

##  Notes

- This is a **live branch** — changes here reflect directly on the website
- Please make sure to follow the project rules before submitting any content

---

*Built with care for the Zith community.*
