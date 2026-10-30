/* Pacitum 2.0.1: display language for the internal pages (Settings, New tab, Welcome, Extensions).
   window.__I18N is the dictionary of the language in use: { "English text": "translation" }. A key may contain
   %1, %2 ... which stand for any text ("%1 passwords"). Text without an entry stays English.
   Everything that is drawn into the page is translated here, so the pages keep plain English source text. */
(function () {
  'use strict';
  var D = window.__I18N || {};
  var exact = Object.create(null), patterns = [];
  var ph = /%(\d)/g;
  function norm(s) { return s.replace(/\s+/g, ' ').trim(); }
  function esc(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }
  Object.keys(D).forEach(function (k) {
    var v = D[k]; if (!v) return;
    var nk = norm(k);
    if (!/%\d/.test(nk)) { exact[nk] = v; return; }
    var rx = '^', last = 0, nums = [], prefix = null, m;
    ph.lastIndex = 0;
    while ((m = ph.exec(nk))) {
      var lit = nk.slice(last, m.index);
      if (prefix === null) prefix = lit;
      rx += esc(lit) + '(.+?)'; nums.push(+m[1]); last = m.index + m[0].length;
    }
    rx += esc(nk.slice(last)) + '$';
    patterns.push({ prefix: prefix || '', re: new RegExp(rx), nums: nums, out: v });
  });
  var on = Object.keys(D).length > 0;
  var cache = new Map();

  function core(s, depth) {
    var n = norm(s);
    if (exact[n] !== undefined) return exact[n];
    if (n.length < 2 || n.length > 400 || n.charAt(0) === '<') return s;
    for (var i = 0; i < patterns.length; i++) {
      var p = patterns[i];
      if (p.prefix && n.indexOf(p.prefix) !== 0) continue;
      var m = p.re.exec(n);
      if (!m) continue;
      var out = p.out;
      for (var j = 0; j < p.nums.length; j++) out = out.split('%' + p.nums[j]).join(depth < 2 ? core(m[j + 1], depth + 1) : m[j + 1]);
      return out;
    }
    return s;
  }
  // t('Text') / t('%1 passwords', 3): translate a text built in script
  function t(s) {
    if (s == null) return s;
    s = String(s);
    var out = s;
    if (on) {
      if (cache.has(s)) out = cache.get(s);
      else {
        var a = 0, b = s.length;
        while (a < b && /\s/.test(s.charAt(a))) a++;
        while (b > a && /\s/.test(s.charAt(b - 1))) b--;
        out = s.slice(0, a) + core(s.slice(a, b), 0) + s.slice(b);
        if (cache.size > 4000) cache.clear();
        cache.set(s, out);
      }
    }
    for (var i = 1; i < arguments.length; i++) out = out.split('%' + i).join(arguments[i]);
    return out;
  }
  window.t = t;
  window.i18n = { t: t, lang: document.documentElement.lang || 'en', on: on };
  if (!on) return;

  var SKIP = { SCRIPT: 1, STYLE: 1, TEXTAREA: 1, NOSCRIPT: 1, CODE: 1, PRE: 1, SVG: 1, svg: 1, PATH: 1 };
  var ATTRS = ['placeholder', 'title', 'aria-label', 'alt', 'aria-description'];

  function text(node) {
    var v = node.nodeValue;
    if (!v || !/\S/.test(v)) return;
    var p = node.parentNode;
    if (p && (SKIP[p.nodeName] || (p.closest && p.closest('[data-notr]')))) return;
    var src = node.__pmSrc, out = node.__pmOut;
    if (src === undefined || v !== out) src = v;      // new text from the page
    var tr = t(src);
    node.__pmSrc = src; node.__pmOut = tr;
    if (tr !== v) node.nodeValue = tr;
  }
  function attrs(el) {
    if (el.closest && el.closest('[data-notr]')) return;
    for (var i = 0; i < ATTRS.length; i++) {
      var a = ATTRS[i];
      if (!el.hasAttribute(a)) continue;
      var v = el.getAttribute(a);
      var ks = '__pmS_' + a, ko = '__pmO_' + a;
      var src = el[ks]; if (src === undefined || v !== el[ko]) src = v;
      var tr = t(src);
      el[ks] = src; el[ko] = tr;
      if (tr !== v) el.setAttribute(a, tr);
    }
    if (el.nodeName === 'INPUT' && (el.type === 'button' || el.type === 'submit') && el.value) {
      var tv = t(el.value); if (tv !== el.value) el.value = tv;
    }
  }
  function walk(root) {
    if (root.nodeType === 3) { text(root); return; }
    if (root.nodeType !== 1 || SKIP[root.nodeName]) return;
    attrs(root);
    var w = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT, null);
    var n;
    while ((n = w.nextNode())) {
      if (n.nodeType === 3) text(n);
      else if (!SKIP[n.nodeName]) attrs(n);
    }
  }
  var queue = [], scheduled = false;
  function flush() {
    scheduled = false;
    var q = queue; queue = [];
    for (var i = 0; i < q.length; i++) {
      var n = q[i];
      if (n.nodeType === 3) text(n);
      else if (n.isConnected) walk(n);
    }
  }
  function push(n) { queue.push(n); if (!scheduled) { scheduled = true; Promise.resolve().then(flush); } }
  new MutationObserver(function (muts) {
    for (var i = 0; i < muts.length; i++) {
      var m = muts[i];
      if (m.type === 'childList') for (var j = 0; j < m.addedNodes.length; j++) push(m.addedNodes[j]);
      else if (m.type === 'characterData') push(m.target);
      else if (m.type === 'attributes' && m.target.nodeType === 1) push(m.target);
    }
  }).observe(document.documentElement, { childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ATTRS });
  document.addEventListener('DOMContentLoaded', function () { walk(document.documentElement); document.title = t(document.title); });
})();
