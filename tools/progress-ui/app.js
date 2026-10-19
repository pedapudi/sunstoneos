(() => {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const state = { data: null, revision: null, etag: null, view: 'hardware', dependency: 'gpu', expanded: new Set(), request: 0, controller: null, docRequest: 0, docController: null, document: null, history: [], opener: null };
  const evidenceLabels = { board: 'Physical result', host: 'Host checked', emulator: 'Emulator checked', source: 'Source reviewed' };
  const statusLabels = { planned: 'Planned', implemented: 'Partial implementation', host_tested: 'Host / emulator evidence', board_tested: 'Physical result (partial)', qualified: 'Qualified' };
  function el(tag, className, text) { const node = document.createElement(tag); if (className) node.className = className; if (text !== undefined) node.textContent = String(text); return node; }
  function list(value) { return Array.isArray(value) ? value : []; }
  function plain(value) { return String(value || '').replace(/\[([^\]]+)\]\([^)]+\)/g, '$1').replace(/\*\*|__|`/g, '').replace(/\s+/g, ' ').trim(); }
  function excerpt(value, maximum = 240) {
    const text = plain(value), sentence = text.match(/^.*?[.!?](?:\s|$)/)?.[0]?.trim() || text;
    return sentence.length <= maximum ? sentence : sentence.slice(0, maximum).replace(/\s+\S*$/, '') + '…';
  }
  function date(value, includeTime = false) {
    const d = new Date(value);
    if (!value || Number.isNaN(d.getTime())) return 'Time unavailable';
    return new Intl.DateTimeFormat(undefined, includeTime ? { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' } : { month: 'short', day: 'numeric' }).format(d);
  }
  function docPath(value, base = 'docs/hardware.md') {
    if (typeof value !== 'string' || !value || /[\x00-\x1f\\]/.test(value)) return null;
    try {
      const url = new URL(value, location.origin + '/' + base);
      if (url.origin !== location.origin) return null;
      let path;
      if (url.pathname === '/api/document' || url.pathname === '/api/source') path = url.searchParams.get('path');
      else path = decodeURIComponent(url.pathname.slice(1));
      return path && /^docs\/[a-zA-Z0-9_./-]+\.md$/.test(path) && !path.split('/').includes('..') ? path : null;
    } catch { return null; }
  }
  function documentButton(path, title, className = 'document-link') {
    const button = el('button', className, title || path);
    button.type = 'button'; button.dataset.document = path;
    return button;
  }
  function inline(container, text, base = 'docs/hardware.md') {
    const source = String(text || '');
    const pattern = /\[([^\]]+)\]\(([^)]+)\)|\*\*([^*]+)\*\*|`([^`]+)`/g;
    let last = 0, match;
    while ((match = pattern.exec(source))) {
      container.append(document.createTextNode(source.slice(last, match.index)));
      if (match[1]) {
        const path = docPath(match[2], base);
        if (path) container.append(documentButton(path, match[1], 'inline-document'));
        else {
          let url;
          try { url = new URL(match[2]); } catch { url = null; }
          if (url && ['https:', 'http:'].includes(url.protocol)) {
            const link = el('a', '', match[1]); link.href = url.href; link.target = '_blank'; link.rel = 'noopener noreferrer'; container.append(link);
          } else container.append(document.createTextNode(match[1]));
        }
      } else if (match[3]) container.append(el('strong', '', match[3]));
      else container.append(el('code', '', match[4]));
      last = pattern.lastIndex;
    }
    container.append(document.createTextNode(source.slice(last)));
  }
  // This intentionally small Markdown renderer never inserts HTML from a record.
  function markdown(container, text, base = 'docs/hardware.md') {
    const lines = String(text || '').replace(/\r/g, '').split('\n');
    let paragraph = [], items = null, code = null;
    const flush = () => { if (paragraph.length) { const p = el('p'); inline(p, paragraph.join(' '), base); container.append(p); paragraph = []; } };
    const tableCells = (line) => line.trim().replace(/^\|/, '').replace(/\|$/, '').split(/(?<!\\)\|/).map((cell) => cell.trim().replace(/\\\|/g, '|'));
    for (let index = 0; index < lines.length; index++) {
      const line = lines[index];
      if (/^\s*```/.test(line)) {
        flush(); items = null;
        if (code !== null) { const pre = el('pre'); pre.append(el('code', '', code.join('\n'))); container.append(pre); code = null; }
        else code = [];
        continue;
      }
      if (code !== null) { code.push(line); continue; }
      if (!line.trim()) { flush(); items = null; continue; }
      if (line.includes('|') && index + 1 < lines.length && tableCells(lines[index + 1]).every((cell) => /^:?-{3,}:?$/.test(cell))) {
        flush(); items = null;
        const wrapper = el('div', 'table-scroll'), table = el('table'), head = el('thead'), header = el('tr'), body = el('tbody');
        wrapper.tabIndex = 0; wrapper.setAttribute('role', 'region'); wrapper.setAttribute('aria-label', 'Document table');
        const headings = tableCells(line);
        for (const title of headings) { const cell = el('th'); cell.scope = 'col'; inline(cell, title, base); header.append(cell); }
        head.append(header); table.append(head, body); index++;
        while (index + 1 < lines.length && lines[index + 1].trim() && lines[index + 1].includes('|')) {
          const values = tableCells(lines[++index]), row = el('tr');
          for (let column = 0; column < headings.length; column++) { const cell = el('td'); inline(cell, values[column] || '', base); row.append(cell); }
          body.append(row);
        }
        wrapper.append(table); container.append(wrapper); continue;
      }
      const heading = line.match(/^(#{1,6})\s+(.+)$/);
      if (heading) { flush(); items = null; const h = el('h' + Math.min(heading[1].length, 4)); inline(h, heading[2], base); container.append(h); continue; }
      const item = line.match(/^\s*(?:([-*])|\d+\.)\s+(.+)$/);
      if (item) {
        flush(); const tag = item[1] ? 'ul' : 'ol';
        if (!items || items.tagName.toLowerCase() !== tag) { items = el(tag); container.append(items); }
        const li = el('li'); inline(li, item[2], base); items.append(li); continue;
      }
      if (items && /^\s+\S/.test(line)) { items.lastElementChild.append(document.createTextNode(' ')); inline(items.lastElementChild, line.trim(), base); continue; }
      items = null; paragraph.push(line.trim());
    }
    flush();
    if (code !== null) container.append(el('pre', '', code.join('\n')));
  }
  function linkButtons(container, links) {
    const seen = new Set();
    for (const link of list(links)) {
      const path = docPath(link.path ? '/' + link.path : link.url);
      if (!path || seen.has(path)) continue;
      seen.add(path); container.append(documentButton(path, link.label || link.title || path));
    }
  }
  function detailBlock(container, title, content) {
    if (!content) return;
    const section = el('section', 'detail-block prose'); section.append(el('h4', '', title)); markdown(section, content); container.append(section);
  }
  function hardwareCard(item) {
    const card = el('details', 'hardware-card'); card.dataset.hardwareId = item.id; card.open = state.expanded.has(item.id);
    const summary = el('summary');
    const top = el('div', 'card-top'); top.append(el('span', 'card-area', item.area || 'Hardware'), el('span', 'card-toggle', '+')); top.lastChild.setAttribute('aria-hidden', 'true');
    summary.append(top, el('h3', 'card-name', item.name), el('p', 'card-summary', plain(item.functions || item.summary)));
    const badges = el('div', 'card-badges');
    badges.append(el('span', 'pill ' + (item.implementation === 'implemented' ? 'implemented' : ''), item.implementation === 'implemented' ? 'Partial implementation' : item.implementation === 'planned' ? 'Planned' : 'Status unavailable'));
    for (const kind of list(item.evidence)) if (evidenceLabels[kind]) badges.append(el('span', 'pill ' + kind, evidenceLabels[kind]));
    summary.append(badges, el('span', 'qualification-line', item.qualification === 'qualified' ? 'Qualified within documented criteria' : item.qualification === 'unqualified' ? 'Qualification pending' : 'Qualification not recorded'));
    const detail = el('div', 'card-detail');
    detailBlock(detail, 'Required functions', item.functions);
    detailBlock(detail, 'Dependencies', item.dependencies);
    if (list(item.details).length) for (const entry of item.details) detailBlock(detail, entry.title || 'Evidence and limits', entry.markdown);
    else if (item.implementation === 'planned' && !list(item.evidence).some((kind) => ['board', 'host', 'emulator'].includes(kind))) detailBlock(detail, 'Evidence and limits', 'No implementation or test evidence is recorded for this item.');
    else detailBlock(detail, 'Evidence and limits', item.summary || 'Detailed evidence is unavailable. Read the support matrix for the recorded status.');
    const links = el('div', 'document-links'); linkButtons(links, item.links); detail.append(links); card.append(summary, detail);
    card.addEventListener('toggle', () => { if (card.open) state.expanded.add(item.id); else state.expanded.delete(item.id); });
    return card;
  }
  function syncFilter(id, values, label) {
    const select = $(id), previous = select.value;
    select.replaceChildren(new Option(label, ''));
    for (const [value, text] of values) select.append(new Option(text, value));
    if ([...select.options].some((option) => option.value === previous)) select.value = previous;
  }
  function hardware() {
    const items = list(state.data?.hardware), search = $('search').value.toLocaleLowerCase().trim(), area = $('area').value, status = $('status').value;
    const tokens = search.split(/\s+/).filter(Boolean);
    const matches = items.filter((item) => (!area || item.area === area) && (!status || (status === 'qualified' ? item.qualification === status : item.status === status)) && tokens.every((word) => plain([item.name, item.area, item.functions, item.dependencies, item.summary, ...list(item.details).map((part) => part.markdown)].join(' ')).toLocaleLowerCase().includes(word)));
    const grid = $('hardware-grid');
    const activeId = document.activeElement?.closest('[data-hardware-id]')?.dataset.hardwareId;
    grid.replaceChildren(...matches.map(hardwareCard)); grid.setAttribute('aria-busy', 'false');
    $('empty').hidden = matches.length !== 0;
    $('result-count').textContent = `${matches.length} of ${items.length} hardware areas`;
    $('clear-filters').hidden = !search && !area && !status;
    if (activeId) [...grid.children].find((node) => node.dataset.hardwareId === activeId)?.querySelector('summary')?.focus({ preventScroll: true });
  }
  function nextActions() {
    const data = state.data, actions = $('next-actions'); actions.replaceChildren();
    for (const [index, item] of list(data.handoff?.next_actions).entries()) {
      const row = el('details', 'action'), summary = el('summary'), text = el('div', 'prose action-detail');
      summary.append(el('span', 'action-number', String(item.order || index + 1).padStart(2, '0')), el('span', 'action-excerpt', excerpt(item.text)), el('span', 'action-expand', '+'));
      summary.lastChild.setAttribute('aria-hidden', 'true'); markdown(text, item.text, 'docs/handoff.md'); row.append(summary, text); actions.append(row);
    }
    if (!actions.children.length) actions.append(el('p', 'secondary', 'No next actions are available in the current record.'));
    const blockers = $('blockers'); blockers.replaceChildren();
    for (const item of list(data.handoff?.blockers)) {
      const content = typeof item === 'string' ? item : item.text;
      const row = el('details', 'blocker'), summary = el('summary', '', excerpt(content, 170)), body = el('div', 'prose');
      markdown(body, content, 'docs/handoff.md');
      const path = docPath(item.source?.path ? '/' + item.source.path : typeof item.source === 'string' ? '/' + item.source : '');
      if (path) body.append(documentButton(path, 'Read source'));
      row.append(summary, body); blockers.append(row);
    }
    if (!blockers.children.length) blockers.append(el('p', 'secondary', 'No separate constraint summary is available. See current work for conditions and limits.'));
    const mimus = $('mimus-summary'); mimus.replaceChildren(); markdown(mimus, data.mimus?.summary || 'Application evidence is unavailable.', 'docs/mimus.md');
    const queue = data.queue || {};
    $('queue-state').textContent = !queue.available ? 'Unavailable' : queue.halted ? 'Paused' : `${list(queue.items).length} requests`;
    $('queue-state').className = 'pill' + (queue.halted ? ' warning' : '');
    $('queue-note').textContent = [queue.reason, queue.note].filter(Boolean).join(' ') || 'Read-only view of scheduled physical checks.';
    const queueList = $('queue-list'); queueList.replaceChildren();
    for (const item of list(queue.items)) {
      const row = el('article', 'queue-item'), meta = el('div', 'queue-meta');
      row.append(el('h4', '', item.title || item.id)); meta.append(el('span', 'pill', String(item.status || 'Unknown').replace(/_/g, ' ')));
      if (item.owner) meta.append(el('span', '', item.owner)); row.append(meta); queueList.append(row);
    }
    if (queue.available && !queueList.children.length) queueList.append(el('p', 'secondary', 'No board tests are in the queue.'));
  }
  function activity() {
    const git = state.data.git || {}, container = $('activity-list'); container.replaceChildren();
    $('branch-name').textContent = git.available ? git.branch || '' : 'History unavailable';
    $('git-note').textContent = git.note || 'Recent repository activity is shown as recorded.';
    for (const item of list(git.commits)) {
      const row = el('li', 'activity-item'), time = el('time', '', date(item.time));
      if (item.time) time.dateTime = item.time;
      time.title = date(item.time, true);
      row.append(time, el('span', 'activity-subject', item.subject), el('span', 'activity-hash', item.short_hash || String(item.hash || '').slice(0, 7))); container.append(row);
    }
    if (!container.children.length) container.append(el('li', 'activity-item secondary', 'Recent activity is unavailable.'));
    const sources = $('sources'); sources.replaceChildren();
    for (const source of list(state.data.sources)) {
      const path = docPath('/' + source.path); if (!path) continue;
      const button = documentButton(path, source.label || source.path); button.disabled = source.available === false;
      if (button.disabled) button.title = 'Source unavailable'; sources.append(button);
    }
  }
  function notice(message) { $('notice').textContent = message || ''; $('notice').hidden = !message; }
  function render(data) {
    const items = list(data.hardware), summary = data.summary || {};
    $('project-summary').textContent = plain(summary.detail || summary.headline || 'Read the source records for current implementation and qualification evidence.');
    $('count-total').textContent = items.length;
    $('count-board').textContent = items.filter((item) => list(item.evidence).includes('board')).length;
    $('count-checked').textContent = items.filter((item) => list(item.evidence).some((kind) => kind === 'host' || kind === 'emulator')).length;
    $('count-qualified').textContent = typeof summary.qualified === 'number' ? summary.qualified : '—';
    $('count-qualified').title = typeof summary.qualified === 'number' ? 'Recorded qualified hardware areas' : 'Qualification status unknown';
    $('inventory-count').textContent = items.length; $('metrics').setAttribute('aria-busy', 'false');
    const firstParagraph = String(data.mimus?.summary || '').split(/\n\s*\n/)[0];
    const sentences = plain(firstParagraph).match(/[^.!?]+[.!?]+(?:\s|$)|[^.!?]+$/g) || [];
    $('featured-mimus').textContent = sentences.length ? sentences.slice(0, 2).join(' ').trim() : 'Application evidence is unavailable. Open the record for details.';
    const areas = [...new Set(items.map((item) => item.area).filter(Boolean))].sort();
    syncFilter('area', areas.map((value) => [value, value]), 'All areas');
    const statuses = [...new Set(items.map((item) => item.status).filter(Boolean))];
    const options = Object.entries(statusLabels).filter(([key]) => statuses.includes(key) || key === 'qualified' && items.some((item) => item.qualification === key));
    for (const value of statuses) if (!statusLabels[value]) options.push([value, value.replace(/_/g, ' ')]);
    syncFilter('status', options, 'All statuses'); hardware(); dependencies(); nextActions(); activity();
    const performance = summary.performance_note || data.performance_note;
    $('performance-note').hidden = !performance; $('performance-copy').textContent = plain(performance);
    $('source-time').textContent = `Records updated ${date(data.updated_at, true)} · Checks every 30 seconds while visible`;
    $('source-time').title = data.updated_at || '';
  }
  function dependencies() {
    const graph = state.data?.dependencies, canvas = $('dependency-graph'), detail = $('dependency-detail');
    canvas.replaceChildren(); detail.replaceChildren();
    if (graph?.status !== 'ok') {
      $('dependency-summary').textContent = 'Dependency graph unavailable. ' + list(graph?.warnings).join(' ');
      $('dependency-focus').replaceChildren(); return;
    }
    const nodes = graph.nodes, byId = new Map(nodes.map((node) => [node.id, node]));
    if (!byId.has(state.dependency)) state.dependency = nodes[0].id;
    const select = $('dependency-focus'); select.replaceChildren();
    for (const node of nodes) { const option = el('option', '', node.name); option.value = node.id; select.append(option); }
    select.value = state.dependency;
    const selected = byId.get(state.dependency), ancestors = new Set(), descendants = new Set();
    function visit(id) { for (const parent of byId.get(id).requires) if (!ancestors.has(parent)) { ancestors.add(parent); visit(parent); } }
    visit(selected.id);
    for (const node of nodes) if (node.requires.includes(selected.id)) descendants.add(node.id);
    const all = $('dependency-all').checked;
    const visible = nodes.filter((node) => all || node.id === selected.id || ancestors.has(node.id) || descendants.has(node.id));
    $('dependency-summary').textContent = `${graph.hardware_count} hardware areas · ${nodes.length} functions and shared prerequisites · ${visible.length} shown. ${all ? 'Complete graph; scroll horizontally to explore.' : 'Showing all prerequisites and direct dependents of ' + selected.name + '. Scroll horizontally to follow the path.'}`;
    // Compact the levels in a focused view; retain topological order.
    const levels = [...new Set(visible.map((node) => node.level))].sort((a, b) => a - b), positions = new Map();
    const width = 218, gap = 54, height = 92, rowGap = 16;
    let rows = 1;
    levels.forEach((level, column) => {
      const group = visible.filter((node) => node.level === level); rows = Math.max(rows, group.length);
      const heading = el('span', 'dependency-column', `DEPTH ${level + 1}`);
      heading.style.left = `${column * (width + gap) + 20}px`; canvas.append(heading);
      group.forEach((node, row) => positions.set(node.id, { x: column * (width + gap) + 20, y: row * (height + rowGap) + 44 }));
    });
    const totalWidth = levels.length * (width + gap) - gap + 40, totalHeight = rows * (height + rowGap) + 48;
    canvas.style.width = `${totalWidth}px`; canvas.style.height = `${totalHeight}px`;
    const ns = 'http://www.w3.org/2000/svg', svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('width', totalWidth); svg.setAttribute('height', totalHeight); svg.setAttribute('aria-hidden', 'true');
    const defs = document.createElementNS(ns, 'defs'), marker = document.createElementNS(ns, 'marker');
    marker.id = 'dependency-arrow'; marker.setAttribute('viewBox', '0 0 10 10'); marker.setAttribute('refX', '9'); marker.setAttribute('refY', '5'); marker.setAttribute('markerWidth', '5'); marker.setAttribute('markerHeight', '5'); marker.setAttribute('orient', 'auto');
    const arrow = document.createElementNS(ns, 'path'); arrow.setAttribute('d', 'M 0 0 L 10 5 L 0 10 z'); arrow.setAttribute('fill', '#6b8175'); marker.append(arrow); defs.append(marker); svg.append(defs);
    for (const edge of graph.edges) {
      if (!positions.has(edge.from) || !positions.has(edge.to)) continue;
      // Focused graphs show only the dependency closure and selected node's outgoing edges.
      if (!all && descendants.has(edge.to) && edge.from !== selected.id) continue;
      const start = positions.get(edge.from), end = positions.get(edge.to), x = start.x + width, y = start.y + height / 2, toY = end.y + height / 2;
      const path = document.createElementNS(ns, 'path');
      path.setAttribute('d', `M ${x} ${y} C ${x + gap / 2} ${y}, ${end.x - gap / 2} ${toY}, ${end.x} ${toY}`);
      path.setAttribute('class', 'dependency-edge' + (edge.from === selected.id ? ' downstream' : ''));
      path.setAttribute('marker-end', 'url(#dependency-arrow)'); svg.append(path);
    }
    canvas.prepend(svg);
    function choose(id) { state.dependency = id; dependencies(); }
    for (const node of visible) {
      const pos = positions.get(node.id), active = node.id === selected.id;
      const button = el('button', 'dependency-node' + (active ? ' selected' : '') + (descendants.has(node.id) ? ' dependent' : ''));
      button.type = 'button'; button.dataset.node = node.id; button.setAttribute('aria-pressed', String(active));
      button.style.left = `${pos.x}px`; button.style.top = `${pos.y}px`;
      button.append(el('strong', '', node.name), el('span', 'dependency-node-status', node.hardware_id ? (statusLabels[node.status] || node.status_label) : 'Shared function / milestone'));
      button.addEventListener('click', () => { choose(node.id); canvas.querySelector(`[data-node="${node.id}"]`)?.focus({ preventScroll: true }); });
      canvas.append(button);
    }
    detail.append(el('p', 'eyebrow', selected.hardware_id ? 'HARDWARE FUNCTION' : 'SHARED FUNCTION / MILESTONE'), el('h3', '', selected.name), el('p', '', selected.scope));
    const area = list(state.data.hardware).find((item) => item.id === selected.id);
    if (area) {
      detail.append(el('h4', '', 'Recorded evidence'), el('p', 'secondary', (statusLabels[area.status] || area.status_label) + '. Applies only to the scope in the support matrix.'), el('p', 'secondary', 'Qualification: ' + area.qualification.replace(/_/g, ' ') + '.'));
      const evidence = el('details', 'dependency-evidence'); evidence.append(el('summary', '', 'Read current scope'));
      const copy = el('div', 'prose'); markdown(copy, area.details.map((part) => part.markdown).join('\n\n')); evidence.append(copy); detail.append(evidence);
    } else detail.append(el('p', 'secondary', 'This node describes required functionality. Its component record holds the evidence; no separate completion status is inferred.'));
    for (const [title, ids] of [['Direct prerequisites', selected.requires], ['Direct dependents', [...descendants]]]) {
      detail.append(el('h4', '', title));
      if (!ids.length) detail.append(el('p', 'secondary', 'None in this graph.'));
      const links = el('div', 'dependency-links');
      for (const id of ids) { const button = el('button', 'text-button', byId.get(id).name); button.type = 'button'; button.addEventListener('click', () => choose(id)); links.append(button); }
      detail.append(links);
    }
    detail.append(el('h4', '', 'Physical qualification needs'), el('p', '', selected.conditions), documentButton(selected.record, 'Open component record ↗'), documentButton('docs/hardware.md', 'Open support matrix ↗'));
  }

  function setConnection(kind, label) { $('connection').className = 'connection ' + kind; $('connection-label').textContent = label; }
  async function refresh() {
    const request = ++state.request;
    state.controller?.abort(); const controller = new AbortController(); state.controller = controller;
    $('refresh').disabled = true;
    const timeout = setTimeout(() => controller.abort(), 15000);
    try {
      const headers = state.etag ? { 'If-None-Match': state.etag } : {};
      const response = await fetch('/api/progress', { headers, signal: state.controller.signal, cache: 'no-cache' });
      if (request !== state.request) return;
      if (response.status !== 304) {
        if (!response.ok) throw new Error(`The server returned ${response.status}.`);
        const data = await response.json();
        if (request !== state.request) return;
        if (data.schema_version !== 1 || !Array.isArray(data.hardware)) throw new Error('The progress record has an unsupported format.');
        if (data.parser?.status && data.parser.status !== 'ok') throw new Error(`Project records could not be interpreted (${data.parser.status}). ${list(data.parser.warnings).join(' ')}`);
        const revision = data.revision || JSON.stringify(data);
        state.etag = response.headers.get('ETag');
        if (revision !== state.revision) { state.data = data; render(data); state.revision = revision; }
      } else if (!state.data) throw new Error('The server returned no initial progress record.');
      notice(list(state.data?.parser?.warnings).join(' ')); setConnection('online', 'Live records');
      $('checked-time').textContent = `Checked ${new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' }).format(new Date())}`;
    } catch (error) {
      if (request !== state.request) return;
      setConnection('error', state.data ? 'Update paused' : 'Unavailable');
      notice(`${state.data ? 'Showing the last successful record. ' : ''}${error.name === 'AbortError' ? 'The update timed out.' : error.message} Use Refresh to try again.`);
      if (!state.data) {
        $('hardware-grid').replaceChildren(el('div', 'loading-state', 'Progress could not be loaded. The source records have not been replaced with estimated statuses.'));
        $('hardware-grid').setAttribute('aria-busy', 'false'); $('metrics').setAttribute('aria-busy', 'false');
        $('result-count').textContent = 'Inventory unavailable';
      }
    } finally { clearTimeout(timeout); if (request === state.request) $('refresh').disabled = false; }
  }
  function view(name) {
    if (!['hardware', 'dependencies', 'next', 'activity'].includes(name)) return;
    state.view = name; history.replaceState(null, '', '#' + name);
    for (const button of document.querySelectorAll('[data-view]')) { const active = button.dataset.view === name; button.classList.toggle('active', active); button.setAttribute('aria-pressed', String(active)); }
    for (const key of ['hardware', 'dependencies', 'next', 'activity']) $('view-' + key).hidden = key !== name;
  }
  function showDocumentText() {
    const container = $('document-body'); container.replaceChildren();
    if (!state.document) return;
    if ($('raw-source').checked) container.append(el('pre', 'raw-document', state.document.content));
    else markdown(container, state.document.content, state.document.path);
  }
  async function openDocument(path, addHistory = true) {
    const safePath = docPath('/' + path); if (!safePath) return;
    const dialog = $('document-dialog');
    if (!dialog.open) { state.opener = document.activeElement; state.history = []; dialog.showModal(); }
    if (addHistory && state.document?.path && state.document.path !== safePath) state.history.push(state.document.path);
    const request = ++state.docRequest;
    state.docController?.abort(); const controller = new AbortController(); state.docController = controller;
    state.document = null;
    $('document-title').textContent = safePath.split('/').pop().replace(/\.md$/, '').replace(/-/g, ' ');
    $('document-path').textContent = safePath; $('document-body').replaceChildren(el('p', 'document-loading', 'Loading the project record…')); $('document-body').setAttribute('aria-busy', 'true');
    const back = $('document-back'); if (back) { back.hidden = !state.history.length; }
    const timeout = setTimeout(() => controller.abort(), 15000);
    try {
      const response = await fetch('/api/document?path=' + encodeURIComponent(safePath), { signal: controller.signal, cache: 'no-cache' });
      if (!response.ok) throw new Error(`The document is unavailable (${response.status}).`);
      const content = await response.text(); if (request !== state.docRequest) return;
      state.document = { path: safePath, content };
      const title = content.match(/^#\s+(.+)$/m); if (title) $('document-title').textContent = plain(title[1]);
      showDocumentText(); dialog.scrollTop = 0;
    } catch (error) {
      if (request === state.docRequest) $('document-body').replaceChildren(el('p', 'notice', error.name === 'AbortError' ? 'The document request timed out. Close this view and try again.' : error.message));
    } finally { clearTimeout(timeout); if (request === state.docRequest) $('document-body').setAttribute('aria-busy', 'false'); }
  }
  function clearFilters() { $('search').value = ''; $('area').value = ''; $('status').value = ''; hardware(); }
  document.addEventListener('click', (event) => {
    const button = event.target.closest('[data-document]');
    if (button && !button.disabled) openDocument(button.dataset.document);
    const tab = event.target.closest('[data-view]'); if (tab) view(tab.dataset.view);
  });
  $('dependency-focus').addEventListener('change', (event) => { state.dependency = event.target.value; dependencies(); });
  $('dependency-all').addEventListener('change', dependencies);
  $('refresh').addEventListener('click', refresh);
  $('search').addEventListener('input', hardware); $('area').addEventListener('change', hardware); $('status').addEventListener('change', hardware);
  $('clear-filters').addEventListener('click', clearFilters); $('empty-clear').addEventListener('click', clearFilters);
  $('raw-source').addEventListener('change', showDocumentText);
  $('close-document').addEventListener('click', () => $('document-dialog').close());
  $('document-dialog').addEventListener('close', () => { ++state.docRequest; state.docController?.abort(); state.document = null; (state.opener?.isConnected ? state.opener : document.querySelector(`[data-view="${state.view}"]`))?.focus({ preventScroll: true }); });
  const back = el('button', 'text-button', '← Back'); back.id = 'document-back'; back.type = 'button'; back.hidden = true;
  back.addEventListener('click', () => { const path = state.history.pop(); if (path) openDocument(path, false); });
  $('document-path').before(back);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) refresh(); });
  setInterval(() => { if (!document.hidden) refresh(); }, 30000);
  view(location.hash.slice(1) || 'hardware');
  if (!document.hidden) refresh();
})();
