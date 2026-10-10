// Renders site/data.js (written by build.py). No knowledge about the format
// lives here: this file only lays out what the generator decided.
(() => {
  'use strict';

  const SITE = window.OVL_SITE;
  const $ = (sel, root = document) => root.querySelector(sel);

  // Statuses in build.py's order, plus the map-only "no resource" category.
  const CATS = [
    { key: 'extracted', hue: 'extracted', shape: 'solid', label: 'Extrait',
      desc: 'Un extracteur l’écrit dans un format standard (.wav, .tga, .glb, .obj, .json) et le décodage est complet.' },
    { key: 'partial', hue: 'partial', shape: 'half', label: 'Partiel',
      desc: 'Un extracteur s’en occupe, mais le décodage est incomplet ou attend un autre bloc encore fermé.' },
    { key: 'decoded', hue: 'decoded', shape: 'solid', label: 'Documenté, pas extrait',
      desc: 'La structure est connue (docs ou rct3dump), rien n’est écrit sur disque.' },
    { key: 'unknown', hue: 'unknown', shape: 'solid', label: 'Non traité',
      desc: 'Aucun document ni extracteur ne mentionne ce type de ressource.' },
    { key: 'none', hue: 'none', shape: 'hollow', label: 'Sans ressource',
      desc: 'Le fichier n’exporte aucun symbole (ou n’a pas pu être listé).' },
  ];

  // --- helpers -------------------------------------------------------------
  function h(tag, attrs, ...kids) {
    const node = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === false || v == null) continue;
      if (k === 'class') node.className = v;
      else if (k === 'text') node.textContent = v;
      else if (k.startsWith('on')) node.addEventListener(k.slice(2), v);
      else node.setAttribute(k, v === true ? '' : v);
    }
    for (const kid of kids.flat()) if (kid != null && kid !== false) node.append(kid);
    return node;
  }
  const int = (n) => n.toLocaleString('fr-FR');
  const pct = (part, whole) =>
    (whole ? (100 * part) / whole : 0).toLocaleString('fr-FR', { minimumFractionDigits: 1, maximumFractionDigits: 1 }) + ' %';
  function size(n) {
    if (n < 1024) return int(n) + ' o';
    if (n < 1048576) return (n / 1024).toLocaleString('fr-FR', { maximumFractionDigits: 1 }) + ' Ko';
    if (n < 1073741824) return (n / 1048576).toLocaleString('fr-FR', { maximumFractionDigits: 1 }) + ' Mo';
    return (n / 1073741824).toLocaleString('fr-FR', { maximumFractionDigits: 2 }) + ' Go';
  }
  const swatch = (cat) => h('span', { class: `sw h-${cat.hue} ${cat.shape}` });
  const chip = (key) => {
    const cat = CATS.find((c) => c.key === key);
    return h('span', { class: 'state' }, swatch(cat), cat.label);
  };
  function sourceLink(ev) {
    const base = SITE.repo ? `${SITE.repo}/blob/${SITE.branch}/` : '../';
    const short = ev.f.replace(/^docs\//, '').replace(/^extractors\/src\//, '');
    if (ev.k === 'doc') {
      const href = SITE.repo ? `${base}${ev.f}?plain=1#L${ev.l}` : `${base}${ev.f}`;
      return h('a', { href, target: '_blank', rel: 'noopener' },
        h('span', { class: 'k', text: short.replace(/\.md$/, '').replace('RCT3_OVL_FORMAT', 'format').toLowerCase() }),
        `${ev.t} (l. ${ev.l})`);
    }
    const href = SITE.repo ? `${base}${ev.f}#L${ev.l}` : `${base}${ev.f}`;
    return h('a', { href, target: '_blank', rel: 'noopener' }, h('span', { class: 'k', text: 'code' }), `${short}:${ev.l}`);
  }
  const mixBar = (mix, cls = 'mini') => {
    const total = mix.reduce((a, b) => a + b, 0);
    return h('div', { class: cls, title: CATS.slice(0, 4).filter((c, i) => mix[i]).map((c, i) => `${c.label} ${pct(mix[CATS.indexOf(c)], total)}`).join(' · ') },
      CATS.slice(0, 4).map((c, i) => (mix[i] ? h('i', { class: `h-${c.hue} ${c.shape}`, style: `flex:${mix[i]} 0 0` }) : null)));
  };
  const sumMix = (mix) => mix.reduce((a, b) => a + b, 0);

  // --- derived data --------------------------------------------------------
  const pairs = SITE.pairs;
  // Category of a pair: the status most of its resources have; ties go to the worse one.
  const pairCat = pairs.map((p) => {
    const total = sumMix(p.m);
    if (!total) return 4;
    let best = 0;
    for (let i = 1; i < 4; i++) if (p.m[i] >= p.m[best]) best = i;
    return best;
  });
  const pairTags = pairs.map((p) => new Set(Object.keys(p.r)));

  // Folder tree: top-level folder, then the next folder when the pair sits deeper.
  const tree = new Map();
  function nodeOf(map, name, path) {
    if (!map.has(name)) map.set(name, { name, path, pairs: [], bytes: 0, mix: [0, 0, 0, 0], children: new Map() });
    return map.get(name);
  }
  pairs.forEach((p, i) => {
    const parts = p.p.split('/');
    const top = parts.length > 1 ? parts[0] : '(racine)';
    const node = nodeOf(tree, top, top);
    const chain = [node];
    if (parts.length > 2) chain.push(nodeOf(node.children, parts[1], `${parts[0]}/${parts[1]}`));
    for (const n of chain) {
      n.pairs.push(i);
      n.bytes += p.s;
      for (let k = 0; k < 4; k++) n.mix[k] += p.m[k];
    }
  });

  // --- state ---------------------------------------------------------------
  let pickedPair = null;
  let pickedTag = null;
  const shown = new Set(); // map categories kept; empty = all
  let onlyUnknown = false;
  let geo = null;
  let tagSort = { key: 'n', dir: -1 };

  // --- header and summary --------------------------------------------------
  function renderFacts() {
    const total = sumMix(SITE.totals);
    const bytes = pairs.reduce((a, p) => a + p.s, 0);
    const rows = [
      ['Installation', SITE.assets.replace(/^.*\/steamapps\/common\//, '…/')],
      ['Paires d’OVL', int(pairs.length)],
      ['Ressources', int(total)],
      ['Types', int(Object.keys(SITE.tags).length)],
      ['Poids', size(bytes)],
    ];
    $('#facts').replaceChildren(...rows.flatMap(([k, v]) => [h('dt', { text: k }), h('dd', { text: v })]));
  }

  function renderSummary() {
    const t = SITE.totals;
    const total = sumMix(t);
    const biggest = (key) => {
      const i = CATS.findIndex((c) => c.key === key);
      const top = Object.entries(SITE.tags).filter(([, v]) => v.st === key).sort((a, b) => b[1].n - a[1].n)[0];
      return top ? `${top[0]} en tête, ${pct(top[1].n, t[i])} de la part` : '';
    };
    const figures = [
      ['extracted', pct(t[0], total), 'Extrait', biggest('extracted')],
      ['partial', pct(t[1], total), 'Partiel', biggest('partial')],
      ['decoded', pct(t[2], total), 'Documenté, pas extrait', biggest('decoded')],
      ['unknown', pct(t[3], total), 'Non traité', biggest('unknown')],
    ];
    $('#figures').replaceChildren(...figures.map(([hue, value, name, note]) =>
      h('div', { class: `figure h-${hue}` }, h('b', { text: value }), h('span', { text: name }), h('small', { text: note }))));
    const stack = $('#stack');
    stack.setAttribute('aria-label', CATS.slice(0, 4).map((c, i) => `${c.label} ${pct(t[i], total)}`).join(', '));
    stack.replaceChildren(...CATS.slice(0, 4).filter((c, i) => t[i]).map((c) =>
      h('i', { class: `h-${c.hue} ${c.shape}`, style: `flex:${t[CATS.indexOf(c)]} 0 0`, title: `${c.label} : ${pct(t[CATS.indexOf(c)], total)}` })));
    const noneCount = pairCat.filter((c) => c === 4).length;
    $('#legend tbody').replaceChildren(...CATS.map((c, i) =>
      h('tr', {},
        h('td', {}, swatch(c), c.label),
        h('td', { text: c.desc }),
        h('td', { class: 'num', text: i < 4 ? int(t[i]) : `${int(noneCount)} fichiers` }),
        h('td', { class: 'num', text: i < 4 ? pct(t[i], total) : '' }))));
  }

  // --- the map -------------------------------------------------------------
  function mask() {
    if (pickedTag == null && pickedPair == null && !shown.size && !onlyUnknown) return null;
    const keep = new Uint8Array(pairs.length);
    for (let i = 0; i < pairs.length; i++) {
      let ok = true;
      if (pickedPair != null) ok = i === pickedPair;
      if (ok && pickedTag != null) ok = pairTags[i].has(pickedTag);
      if (ok && shown.size) ok = shown.has(pairCat[i]);
      if (ok && onlyUnknown) ok = pairs[i].m[3] > 0;
      keep[i] = ok ? 1 : 0;
    }
    return keep;
  }

  function layoutMap() {
    const stage = $('#map-stage');
    const width = stage.clientWidth;
    if (!width) return;
    const px = width >= 620 ? 7 : 5;
    const cols = Math.max(8, Math.floor(width / px));
    const rows = Math.ceil(pairs.length / cols);
    geo = { px, cols, rows };
    const canvas = $('#map');
    const dpr = window.devicePixelRatio || 1;
    canvas.width = cols * px * dpr;
    canvas.height = rows * px * dpr;
    canvas.style.width = `${cols * px}px`;
    canvas.style.height = `${rows * px}px`;
    const gutter = $('#map-gutter');
    gutter.replaceChildren();
    const step = Math.max(1, Math.round(60 / px));
    for (let r = 0; r < rows; r += step) gutter.append(h('span', { style: `top:${r * px}px`, text: `n° ${int(r * cols + 1)}` }));
    $('#map-scale').textContent = `Une case = une paire .common.ovl / .unique.ovl, dans l’ordre des chemins, colorée selon l’état majoritaire de ses ressources.`;
    drawMap();
    placeLabels();
  }

  function drawMap(hover) {
    if (!geo) return;
    const canvas = $('#map');
    const ctx = canvas.getContext('2d');
    const dpr = window.devicePixelRatio || 1;
    const css = getComputedStyle(document.documentElement);
    // Same hue tokens as the stylesheet: green for anything an extractor writes,
    // blue for documented-only, orange for untouched, neutral for empty files.
    const VAR = { extracted: '--c-decoded', partial: '--c-decoded', decoded: '--c-named', unknown: '--c-unknown', none: '--c-framing' };
    const colour = (hue) => css.getPropertyValue(VAR[hue]).trim();
    const { px, cols } = geo;
    const side = px - 1;
    const keep = mask();
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.lineWidth = 1;
    for (let i = 0; i < pairs.length; i++) {
      const cat = CATS[pairCat[i]];
      const x = (i % cols) * px;
      const y = Math.floor(i / cols) * px;
      ctx.globalAlpha = keep && !keep[i] ? 0.13 : 1;
      ctx.fillStyle = ctx.strokeStyle = colour(cat.hue);
      if (cat.shape === 'solid') ctx.fillRect(x, y, side, side);
      else {
        ctx.strokeRect(x + 0.5, y + 0.5, side - 1, side - 1);
        if (cat.shape === 'half') {
          ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + side, y); ctx.lineTo(x, y + side); ctx.fill();
        }
      }
    }
    ctx.globalAlpha = 1;
    if (hover != null) {
      ctx.strokeStyle = css.getPropertyValue('--ink').trim();
      ctx.lineWidth = 2;
      ctx.strokeRect((hover % cols) * px - 1, Math.floor(hover / cols) * px - 1, px + 1, px + 1);
    }
  }

  function placeLabels() {
    const box = $('#map-labels');
    box.replaceChildren();
    if (!geo) return;
    let lastRow = -10;
    let start = 0;
    const flush = (end) => {
      const count = end - start;
      if (count < pairs.length * 0.02) return;
      const row = Math.floor(Math.floor((start + end) / 2) / geo.cols);
      if (row - lastRow < 3) return;
      lastRow = row;
      const name = pairs[start].p.includes('/') ? pairs[start].p.split('/')[0] : '(racine)';
      box.append(h('span', { style: `top:${row * geo.px - 8}px; left:8px`, text: `${name} · ${int(count)} paires` }));
    };
    const topOf = (p) => (p.p.includes('/') ? p.p.split('/')[0] : '(racine)');
    for (let i = 1; i <= pairs.length; i++) {
      if (i === pairs.length || topOf(pairs[i]) !== topOf(pairs[start])) { flush(i); start = i; }
    }
  }

  function cellAt(event) {
    const rect = $('#map').getBoundingClientRect();
    const col = Math.floor((event.clientX - rect.left) / geo.px);
    const row = Math.floor((event.clientY - rect.top) / geo.px);
    const i = row * geo.cols + col;
    return col >= 0 && col < geo.cols && i >= 0 && i < pairs.length ? i : null;
  }

  const describe = (p) => {
    const total = sumMix(p.m);
    if (!total) return 'aucune ressource exportée';
    const parts = CATS.slice(0, 4).map((c, i) => (p.m[i] ? `${int(p.m[i])} ${c.label.toLowerCase()}` : null)).filter(Boolean);
    return `${int(total)} ressource${total > 1 ? 's' : ''} : ${parts.join(', ')}`;
  };

  function onMapMove(event) {
    const tip = $('#tip');
    const i = geo ? cellAt(event) : null;
    if (i == null) { tip.hidden = true; drawMap(); return; }
    drawMap(i);
    const p = pairs[i];
    tip.replaceChildren(h('b', { text: p.p }), h('code', { text: size(p.s) }), h('span', { text: describe(p) }));
    tip.hidden = false;
    const stage = $('#map-stage').getBoundingClientRect();
    const x = event.clientX - stage.left;
    const y = event.clientY - stage.top;
    tip.style.left = `${Math.max(0, Math.min(x + 14, stage.width - tip.offsetWidth))}px`;
    tip.style.top = `${y + 16}px`;
  }

  function onMapClick(event) {
    const i = geo ? cellAt(event) : null;
    if (i == null) return;
    pickedPair = i;
    renderFilters();
    drawMap();
    const p = pairs[i];
    $('#readout').replaceChildren(`${p.p} · ${size(p.s)} · ${describe(p)} · `,
      h('a', { href: '#', text: 'voir ses ressources ↓', onclick: (e) => { e.preventDefault(); openPair(i); } }));
  }

  function renderFilters() {
    const box = $('#filters');
    box.replaceChildren();
    CATS.forEach((c, i) => {
      box.append(h('button', { type: 'button', class: 'chip', 'aria-pressed': String(shown.has(i)),
        onclick: () => { shown.has(i) ? shown.delete(i) : shown.add(i); renderFilters(); drawMap(); } }, swatch(c), c.label));
    });
    box.append(h('button', { type: 'button', class: 'chip', 'aria-pressed': String(onlyUnknown),
      onclick: () => { onlyUnknown = !onlyUnknown; renderFilters(); drawMap(); } }, 'contient du non traité'));
    if (pickedTag != null) box.append(h('button', { type: 'button', class: 'chip clear', onclick: () => pickTag(null) }, `type ${pickedTag} ✕`));
    if (pickedPair != null) box.append(h('button', { type: 'button', class: 'chip clear', onclick: () => { pickedPair = null; renderFilters(); drawMap(); } }, `${pairs[pickedPair].p.split('/').pop()} ✕`));
  }

  function pickTag(tag) {
    pickedTag = tag;
    renderFilters();
    drawMap();
    document.querySelectorAll('#tags tbody tr').forEach((tr) => tr.classList.toggle('lit', tr.dataset.tag === tag));
  }

  // --- resource types ------------------------------------------------------
  function renderTags() {
    const rows = Object.entries(SITE.tags).map(([tag, v]) => ({ tag, ...v }));
    const key = { tag: (r) => r.tag, state: (r) => CATS.findIndex((c) => c.key === r.st), n: (r) => r.n, files: (r) => r.files }[tagSort.key];
    rows.sort((a, b) => {
      const x = key(a); const y = key(b);
      return (x < y ? -1 : x > y ? 1 : 0) * tagSort.dir || b.n - a.n;
    });
    $('#tags tbody').replaceChildren(...rows.map((r) =>
      h('tr', { 'data-tag': r.tag, class: pickedTag === r.tag ? 'lit' : '', tabindex: 0,
        onclick: () => pickTag(pickedTag === r.tag ? null : r.tag),
        onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pickTag(pickedTag === r.tag ? null : r.tag); } } },
        h('td', { class: 'name' }, r.tag, r.note ? h('small', { text: r.note }) : null),
        h('td', { class: 'meaning' }, r.name ? h('span', { class: 'mono', text: r.name }) : null,
          r.meaning ? h('small', { text: r.meaning }) : null,
          r.registry ? h('small', { class: 'registry', text: `Registre : ${r.registry}` }) : null),
        h('td', {}, chip(r.st)),
        h('td', { class: 'num', text: int(r.n) }),
        h('td', { class: 'num', text: int(r.files) }),
        h('td', { class: 'src' }, (r.ev || []).map(sourceLink)))));
    document.querySelectorAll('#tags thead button').forEach((b) => {
      if (b.dataset.sort === tagSort.key) b.setAttribute('aria-sort', tagSort.dir < 0 ? 'descending' : 'ascending');
      else b.removeAttribute('aria-sort');
    });
  }

  // --- folders and files ---------------------------------------------------
  const PAGE = 200;

  function nodeRow(node, depth, id) {
    return h('tr', { class: 'row node', 'data-id': id, 'aria-expanded': 'false', tabindex: 0,
      onclick: () => toggleNode(id), onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggleNode(id); } } },
      h('td', { style: `padding-left:${20 + depth * 18}px`, text: node.name }),
      h('td', { class: 'num', text: int(node.pairs.length) }),
      h('td', { class: 'num', text: int(sumMix(node.mix)) }),
      h('td', { class: 'num', text: size(node.bytes) }),
      h('td', {}, mixBar(node.mix)),
      h('td', { class: 'num', text: node.mix[3] ? int(node.mix[3]) : '' }));
  }

  function pairRow(i, depth) {
    const p = pairs[i];
    const unresolved = p.m[3];
    return h('tr', { class: 'row pair', 'data-pair': i, id: `p-${i}`, 'aria-expanded': 'false', tabindex: 0,
      onclick: () => togglePair(i), onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); togglePair(i); } } },
      h('td', { class: 'name', style: `padding-left:${20 + depth * 18}px`, text: p.p.split('/').pop() + (p.e ? ' ⚠' : '') }),
      h('td', { class: 'num' }),
      h('td', { class: 'num', text: int(sumMix(p.m)) }),
      h('td', { class: 'num', text: size(p.s) }),
      h('td', {}, mixBar(p.m)),
      h('td', { class: 'num', text: unresolved ? int(unresolved) : '' }));
  }

  function nodeById(id) {
    const [top, sub] = id.split('\u0000');
    const node = tree.get(top);
    return sub ? node.children.get(sub) : node;
  }

  function childrenRows(id, depth, must) {
    const node = nodeById(id);
    const rows = [];
    const direct = [];
    const inChild = new Set();
    for (const child of node.children.values()) for (const i of child.pairs) inChild.add(i);
    for (const i of node.pairs) if (!inChild.has(i)) direct.push(i);
    for (const [name, child] of [...node.children.entries()].sort((a, b) => b[1].pairs.length - a[1].pairs.length || a[0].localeCompare(b[0]))) {
      rows.push(nodeRow(child, depth, `${id}\u0000${name}`));
    }
    const shownPairs = direct.length > PAGE && !(must != null && direct.includes(must)) ? direct.slice(0, PAGE) : direct;
    for (const i of shownPairs) rows.push(pairRow(i, depth));
    if (shownPairs.length < direct.length) {
      rows.push(h('tr', { class: 'more' }, h('td', { colspan: 6, style: `padding-left:${20 + depth * 18}px` },
        h('button', { type: 'button', class: 'chip', text: `afficher les ${int(direct.length - shownPairs.length)} autres`,
          onclick: (e) => { const tr = e.target.closest('tr'); const rest = direct.slice(PAGE).map((i) => pairRow(i, depth)); tr.replaceWith(...rest); } }))));
    }
    return rows;
  }

  function toggleNode(id, forceOpen, must) {
    const row = $(`#tree tr.node[data-id="${CSS.escape(id)}"]`);
    if (!row) return null;
    const open = row.getAttribute('aria-expanded') === 'true';
    if (open && !forceOpen) {
      row.setAttribute('aria-expanded', 'false');
      let next = row.nextElementSibling;
      while (next && Number(next.dataset.depth) > Number(row.dataset.depth || 0)) { const gone = next; next = next.nextElementSibling; gone.remove(); }
      return row;
    }
    if (open) return row;
    row.setAttribute('aria-expanded', 'true');
    const depth = Number(row.dataset.depth || 0) + 1;
    const rows = childrenRows(id, depth, must);
    rows.forEach((r) => { r.dataset.depth = depth; });
    row.after(...rows);
    return row;
  }

  function togglePair(i, forceOpen) {
    const row = $(`#tree tr.pair[data-pair="${i}"]`);
    if (!row) return;
    const open = row.getAttribute('aria-expanded') === 'true';
    if (open && !forceOpen) { row.setAttribute('aria-expanded', 'false'); row.nextElementSibling.remove(); return; }
    if (open) return;
    row.setAttribute('aria-expanded', 'true');
    const detail = h('tr', { class: 'detail' }, h('td', { colspan: 6 }, renderPair(i)));
    detail.dataset.depth = Number(row.dataset.depth || 0) + 1;
    row.after(detail);
  }

  function renderPair(i) {
    const p = pairs[i];
    const inner = h('div', { class: 'detail-inner' });
    inner.append(h('p', { class: 'hint', text: `${p.p}.common.ovl / .unique.ovl · ${size(p.s)}${p.e ? ' · ovlextract a signalé une erreur en listant ce fichier' : ''}` }));
    const entries = Object.entries(p.r).sort((a, b) => {
      const sa = CATS.findIndex((c) => c.key === SITE.tags[a[0]].st);
      const sb = CATS.findIndex((c) => c.key === SITE.tags[b[0]].st);
      return sa - sb || a[0].localeCompare(b[0]);
    });
    if (!entries.length) inner.append(h('p', { class: 'hint', text: 'Aucun symbole exporté : rien à extraire ici, ou un fichier que le parseur ne lit pas.' }));
    inner.append(h('table', { class: 'grid attrs' },
      h('thead', {}, h('tr', {}, h('th', { text: 'Type' }), h('th', { text: 'État' }), h('th', { class: 'num', text: 'Nombre' }), h('th', { text: 'Ressources' }))),
      h('tbody', {}, entries.map(([tag, names]) => {
        const info = SITE.tags[tag];
        const count = Array.isArray(names) ? names.length : names;
        return h('tr', {},
          h('td', { class: 'name' }, tag, info.name ? h('small', { text: info.name }) : null),
          h('td', {}, chip(info.st)),
          h('td', { class: 'num', text: int(count) }),
          h('td', { class: 'vals' }, Array.isArray(names)
            ? names.map((n) => h('span', { class: 'v', text: n }))
            : h('em', { text: 'noms non embarqués pour ce gros fichier' })));
      }))));
    return inner;
  }

  function renderTree() {
    const body = $('#tree tbody');
    body.replaceChildren();
    const needle = $('#pair-filter').value.trim().toLowerCase();
    if (needle) {
      const hits = [];
      for (let i = 0; i < pairs.length && hits.length < 300; i++) if (pairs[i].p.toLowerCase().includes(needle)) hits.push(i);
      $('#search-hint').textContent = hits.length ? `${hits.length >= 300 ? '300 premiers résultats' : int(hits.length) + ' résultat' + (hits.length > 1 ? 's' : '')} pour « ${needle} »` : `Aucun fichier ne contient « ${needle} »`;
      for (const i of hits) {
        const row = pairRow(i, 0);
        row.firstChild.textContent = pairs[i].p;
        body.append(row);
      }
      return;
    }
    $('#search-hint').textContent = '';
    for (const [name, node] of [...tree.entries()].sort((a, b) => b[1].pairs.length - a[1].pairs.length || a[0].localeCompare(b[0]))) {
      const row = nodeRow(node, 0, name);
      row.dataset.depth = 0;
      body.append(row);
    }
  }

  function openPair(i) {
    if ($('#pair-filter').value) { $('#pair-filter').value = ''; renderTree(); }
    const parts = pairs[i].p.split('/');
    const top = parts.length > 1 ? parts[0] : '(racine)';
    toggleNode(top, true, i);
    if (parts.length > 2) toggleNode(`${top}\u0000${parts[1]}`, true, i);
    togglePair(i, true);
    const row = $(`#tree tr.pair[data-pair="${i}"]`);
    if (row) row.scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  // --- wiring --------------------------------------------------------------
  if (!SITE || !SITE.pairs) {
    $('.lede').textContent = 'Aucune donnée : lancez python3 site/build.py "$ASSETS" pour produire site/data.js.';
    return;
  }
  renderFacts();
  renderSummary();
  renderFilters();
  layoutMap();
  renderTags();
  renderTree();
  $('#stamp').textContent = `Généré le ${SITE.generated} depuis le commit ${SITE.commit || 'inconnu'} (${SITE.branch})` +
    (SITE.dirty ? ', avec des modifications locales de docs/ ou extractors/ non commitées.' : '.');

  $('#map').addEventListener('pointermove', onMapMove);
  $('#map').addEventListener('pointerleave', () => { $('#tip').hidden = true; drawMap(); });
  $('#map').addEventListener('click', onMapClick);
  $('#pair-filter').addEventListener('input', renderTree);
  document.querySelectorAll('#tags thead button').forEach((button) => {
    button.addEventListener('click', () => {
      const key = button.dataset.sort;
      tagSort = { key, dir: tagSort.key === key ? -tagSort.dir : key === 'tag' || key === 'state' ? 1 : -1 };
      renderTags();
    });
  });
  new ResizeObserver(() => {
    const width = $('#map-stage').clientWidth;
    if (geo && Math.max(8, Math.floor(width / (width >= 620 ? 7 : 5))) === geo.cols) return;
    layoutMap();
  }).observe($('#map-stage'));
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => drawMap());
  new MutationObserver(() => drawMap()).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(placeLabels);
})();
