// Pacitum form helper (2.0.1). Runs in an isolated script world: the page's own scripts can't call
// it or read what it holds. It talks to the browser through console messages that start with a
// per-page secret (__PM_NONCE__), and the browser answers by calling window.__pmData(...).
(function () {
  'use strict';
  if (window.__pmAgent) return;
  window.__pmAgent = true;
  var NONCE = '__PM_NONCE__';
  function send(o) { try { console.log(NONCE + JSON.stringify(o)); } catch (e) {} }

  var data = { accounts: [], auto: false, suggest: true, addresses: [], cards: [], fillAddresses: false, fillCards: false, offer: true };
  var autoDone = false;

  // ---------------------------------------------------------------- fields
  function visible(el) {
    if (!el || el.disabled || el.readOnly) return false;
    var r = el.getBoundingClientRect();
    if (r.width < 2 || r.height < 2) return false;
    var cs = getComputedStyle(el);
    return cs.visibility !== 'hidden' && cs.display !== 'none';
  }
  function ac(el) { return ((el.getAttribute('autocomplete') || '') + '').toLowerCase(); }
  function hint(el) { return ((el.name || '') + ' ' + (el.id || '') + ' ' + (el.getAttribute('placeholder') || '') + ' ' + (el.getAttribute('aria-label') || '')).toLowerCase(); }
  function isNewPw(el, all) {
    var a = ac(el), h = hint(el);
    if (a.indexOf('new-password') >= 0) return true;
    if (a.indexOf('current-password') >= 0) return false;
    if (/new|confirm|repeat|again|retype|verify|create|signup|register/.test(h)) return true;
    return all && all.length >= 2;
  }
  function pwFields(root) {
    return Array.prototype.filter.call((root || document).querySelectorAll('input[type=password]'), visible);
  }
  function textInputs(root) {
    return Array.prototype.filter.call((root || document).querySelectorAll('input:not([type]),input[type=text],input[type=email],input[type=tel],input[type=search]'), visible);
  }
  function userFieldFor(pw) {
    var root = pw.form || document;
    var tagged = Array.prototype.filter.call(root.querySelectorAll('input'), function (i) { return /username|email/.test(ac(i)) && visible(i); });
    if (tagged.length) return tagged[0];
    var list = textInputs(root), best = null;
    for (var i = 0; i < list.length; i++) {
      if (list[i].compareDocumentPosition(pw) & Node.DOCUMENT_POSITION_FOLLOWING) best = list[i];
    }
    return best;
  }
  function setValue(el, v) {
    if (!el) return;
    var proto = Object.getPrototypeOf(el);
    var desc = Object.getOwnPropertyDescriptor(proto, 'value') || Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
    try { desc.set.call(el, v); } catch (e) { el.value = v; }
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }

  // Address fields by autocomplete token or by name.
  var ADDR = [
    ['name', /^(name|full-?name)$|\bfull.?name\b|^name$|your.?name/],
    ['given', /given|first.?name|fname/], ['family', /family|last.?name|surname|lname/],
    ['organization', /organi[sz]ation|company/],
    ['street', /street|address.?line.?1|address1|^address$|addr1|street-address/],
    ['street2', /address.?line.?2|address2|addr2|apartment|suite/],
    ['city', /city|town|locality|address-level2/], ['region', /state|province|region|county|address-level1/],
    ['postal', /zip|postal|postcode/], ['country', /country/],
    ['phone', /tel|phone|mobile/], ['email', /e.?mail/]
  ];
  function addrKind(el) {
    var a = ac(el);
    if (a && a !== 'on' && a !== 'off') {
      var map = { 'name': 'name', 'given-name': 'given', 'family-name': 'family', 'organization': 'organization', 'street-address': 'street', 'address-line1': 'street',
        'address-line2': 'street2', 'address-level2': 'city', 'address-level1': 'region', 'postal-code': 'postal', 'country': 'country', 'country-name': 'country', 'tel': 'phone', 'email': 'email' };
      var toks = a.split(/\s+/);
      for (var i = 0; i < toks.length; i++) if (map[toks[i]]) return map[toks[i]];
    }
    if (el.type === 'password' || el.type === 'search') return null;
    var h = hint(el);
    if (/user.?name|login|password|search|captcha|coupon|promo/.test(h)) return null;
    for (var j = 0; j < ADDR.length; j++) if (ADDR[j][1].test(h)) return ADDR[j][0];
    return null;
  }
  function addressFields(root) {
    var out = [];
    Array.prototype.forEach.call((root || document).querySelectorAll('input,select,textarea'), function (el) {
      if (!visible(el)) return;
      var k = addrKind(el); if (k) out.push([k, el]);
    });
    return out;
  }
  function isCardNumber(el) { var a = ac(el), h = hint(el); return a.indexOf('cc-number') >= 0 || /card.?number|cardnum|cc.?num|ccnumber/.test(h); }

  // ---------------------------------------------------------------- dropdown (closed shadow root)
  var host = null, shadow = null, menu = null, menuFor = null;
  function ensureHost() {
    if (host && host.isConnected) return;
    host = document.createElement('pacitum-autofill');
    host.style.cssText = 'all:initial;position:fixed;left:0;top:0;width:0;height:0;z-index:2147483647';
    shadow = host.attachShadow({ mode: 'closed' });
    var dark = window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches;
    var st = document.createElement('style');
    st.textContent =
      '.m{position:fixed;min-width:240px;max-width:360px;padding:6px;border-radius:14px;font:14px Rubik,"Segoe UI",system-ui,sans-serif;' +
      (dark ? 'background:#1B1617;color:#F6F1EF;box-shadow:0 0 0 1px #322B2C,0 12px 32px rgba(0,0,0,.45)' : 'background:#FFFFFF;color:#151112;box-shadow:0 0 0 1px #E2DBD6,0 12px 32px rgba(21,17,18,.16)') + '}' +
      '.i{display:flex;align-items:center;gap:10px;width:100%;min-height:44px;padding:6px 10px;border:0;border-radius:10px;background:transparent;color:inherit;font:inherit;text-align:left;cursor:pointer}' +
      '.i:hover,.i:focus{background:' + (dark ? '#2A2324' : '#EFE9E5') + ';outline:0}' +
      '.a{width:28px;height:28px;border-radius:9px;display:flex;align-items:center;justify-content:center;font-weight:600;font-size:13px;flex:none;background:' + (dark ? '#F6F1EF' : '#151112') + ';color:' + (dark ? '#141011' : '#F7F4F1') + '}' +
      '.t{display:flex;flex-direction:column;min-width:0}.t b{font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}' +
      '.t span{font-size:12.5px;color:' + (dark ? '#A89E9B' : '#6E6561') + ';white-space:nowrap;overflow:hidden;text-overflow:ellipsis}' +
      '.f{font-size:12px;color:' + (dark ? '#A89E9B' : '#6E6561') + ';padding:6px 10px 4px}';
    shadow.appendChild(st);
    (document.documentElement || document.body).appendChild(host);
  }
  function closeMenu() { if (menu) { menu.remove(); menu = null; menuFor = null; } }
  function openMenu(anchor, items, footer) {
    ensureHost(); closeMenu();
    if (!items.length) return;
    menu = document.createElement('div'); menu.className = 'm'; menuFor = anchor;
    items.forEach(function (it) {
      var b = document.createElement('button'); b.className = 'i'; b.type = 'button';
      var av = document.createElement('span'); av.className = 'a'; av.textContent = it.badge || '•';
      var t = document.createElement('span'); t.className = 't';
      var x = document.createElement('b'); x.textContent = it.title; t.appendChild(x);
      if (it.sub) { var y = document.createElement('span'); y.textContent = it.sub; t.appendChild(y); }
      b.appendChild(av); b.appendChild(t);
      b.addEventListener('mousedown', function (e) { e.preventDefault(); });
      b.addEventListener('click', function (e) { e.preventDefault(); closeMenu(); it.run(); });
      menu.appendChild(b);
    });
    if (footer) { var f = document.createElement('div'); f.className = 'f'; f.textContent = footer; menu.appendChild(f); }
    shadow.appendChild(menu);
    var r = anchor.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(r.left, innerWidth - 368)) + 'px';
    var top = r.bottom + 6;
    menu.style.top = top + 'px';
    var mh = menu.getBoundingClientRect().height;
    if (top + mh > innerHeight - 8) menu.style.top = Math.max(8, r.top - mh - 6) + 'px';
  }
  document.addEventListener('mousedown', function (e) { if (menu && e.target !== menuFor && e.target !== host) closeMenu(); }, true);
  document.addEventListener('keydown', function (e) { if (e.key === 'Escape') closeMenu(); }, true);
  addEventListener('scroll', function () { if (menu) closeMenu(); }, true);
  addEventListener('resize', closeMenu);

  // ---------------------------------------------------------------- sign-in fill
  function fillAccount(acc, pw) {
    var p = pw || pwFields().filter(function (f) { return !isNewPw(f, pwFields(f.form || document)); })[0];
    if (!p) return false;
    var u = userFieldFor(p);
    if (u && acc.u) setValue(u, acc.u);
    setValue(p, acc.p);
    send({ t: 'used', id: acc.id });
    return true;
  }
  function accountItems(pw) {
    return data.accounts.map(function (a) {
      return { badge: (a.u || '?').charAt(0).toUpperCase(), title: a.u || '(no username)', sub: '••••••••', run: function () { fillAccount(a, pw); } };
    });
  }
  function fillOnLoad() {
    if (!data.accounts.length) return;
    var pws = pwFields();
    var login = pws.filter(function (f) { return !isNewPw(f, pwFields(f.form || document)); });
    if (!login.length) return;
    var p = login[0];
    if (p.value) return;
    var a = data.accounts[0];
    fillAccount(a, p);
    if (data.auto && data.accounts.length === 1 && !autoDone) {
      autoDone = true;
      var key = '__pacitumAutoSignIn';
      var last = 0; try { last = +sessionStorage.getItem(key) || 0; } catch (e) {}
      if (Date.now() - last > 60000 && p.form) {   // never loop on a wrong password
        try { sessionStorage.setItem(key, String(Date.now())); } catch (e) {}
        setTimeout(function () { try { p.form.requestSubmit ? p.form.requestSubmit() : p.form.submit(); } catch (e) {} }, 300);
      }
    }
  }

  // ---------------------------------------------------------------- strong password suggestion
  function strong() {
    var sets = ['abcdefghijkmnopqrstuvwxyz', 'ABCDEFGHJKLMNPQRSTUVWXYZ', '23456789', '-_.!?#'];
    var all = sets.join(''), out = [], buf = new Uint32Array(18);
    crypto.getRandomValues(buf);
    for (var i = 0; i < 16; i++) out.push(all[buf[i] % all.length]);
    for (var j = 0; j < sets.length; j++) out[(buf[16] + j * 5) % 16] = sets[j][buf[17] % sets[j].length];
    return out.join('');
  }

  // ---------------------------------------------------------------- focus: open the right menu
  document.addEventListener('focusin', function (e) {
    var el = e.target;
    if (!(el instanceof HTMLInputElement) && !(el instanceof HTMLSelectElement) && !(el instanceof HTMLTextAreaElement)) return;
    if (!visible(el)) return;
    if (el.type === 'password') {
      var all = pwFields(el.form || document);
      if (isNewPw(el, all)) {
        if (!data.suggest || el.value) return;
        var pw = strong();
        openMenu(el, [{ badge: '★', title: 'Use a strong password', sub: pw, run: function () {
          all.filter(function (f) { return isNewPw(f, all); }).forEach(function (f) { setValue(f, pw); });
        } }], 'Pacitum will save it when you finish signing up.');
        return;
      }
      if (data.accounts.length > 1 || (data.accounts.length === 1 && !el.value)) openMenu(el, accountItems(el), 'Saved passwords for this site');
      return;
    }
    // username field of a sign-in form
    var pws = pwFields(el.form || document);
    if (pws.length && data.accounts.length && userFieldFor(pws[0]) === el) { openMenu(el, accountItems(pws[0]), 'Saved passwords for this site'); return; }
    if (data.fillCards && data.cards.length && isCardNumber(el)) {
      openMenu(el, data.cards.map(function (c) { return { badge: '▭', title: c.masked, sub: c.sub, run: function () { send({ t: 'card', id: c.id }); window.__pmCardTarget = el; } }; }), 'Saved cards');
      return;
    }
    if (data.fillAddresses && data.addresses.length && addrKind(el)) {
      var fields = addressFields(el.form || document);
      if (fields.length < 2) return;
      openMenu(el, data.addresses.map(function (a) {
        return { badge: (a.name || '?').charAt(0).toUpperCase(), title: a.name || a.street, sub: a.line, run: function () { fillAddress(a, fields); } };
      }), 'Saved addresses');
    }
  }, true);

  function fillAddress(a, fields) {
    var parts = (a.name || '').split(/\s+/);
    var v = { name: a.name, given: parts[0] || '', family: parts.slice(1).join(' '), organization: a.organization, street: a.street, street2: '',
      city: a.city, region: a.region, postal: a.postal, country: a.country, phone: a.phone, email: a.email };
    fields.forEach(function (f) {
      var val = v[f[0]]; if (!val) return;
      var el = f[1];
      if (el.tagName === 'SELECT') {
        var low = String(val).toLowerCase();
        for (var i = 0; i < el.options.length; i++) {
          var o = el.options[i];
          if (o.value.toLowerCase() === low || o.text.toLowerCase() === low) { el.selectedIndex = i; el.dispatchEvent(new Event('change', { bubbles: true })); break; }
        }
      } else setValue(el, val);
    });
  }

  // ---------------------------------------------------------------- capture on submit
  var lastSent = '';
  function capture(root) {
    if (!data.offer) return;
    var pws = pwFields(root).filter(function (f) { return f.value; });
    if (!pws.length) { captureAddress(root); return; }
    var all = pwFields(root);
    var fresh = pws.filter(function (f) { return isNewPw(f, all); });
    var p = (fresh.length ? fresh[fresh.length - 1] : pws[0]);
    var u = userFieldFor(pws[0]);
    var msg = { t: 'cap', u: u ? u.value : '', p: p.value, newpw: fresh.length > 0 };
    var key = msg.u + '\u0000' + msg.p;
    if (key === lastSent) return;
    lastSent = key;
    send(msg);
  }
  function captureAddress(root) {
    var fields = addressFields(root), a = {};
    fields.forEach(function (f) { var val = (f[1].value || '').trim(); if (val && !a[f[0]]) a[f[0]] = f[1].tagName === 'SELECT' ? (f[1].selectedOptions[0] || {}).text || val : val; });
    var n = Object.keys(a).length;
    if (n < 3 || !(a.street || a.city || a.postal)) return;
    if (!a.name && (a.given || a.family)) a.name = ((a.given || '') + ' ' + (a.family || '')).trim();
    var key = JSON.stringify(a);
    if (key === lastSent) return;
    lastSent = key;
    send({ t: 'addr', a: a });
  }
  document.addEventListener('submit', function (e) { capture(e.target); }, true);
  document.addEventListener('click', function (e) {
    var b = e.target && e.target.closest ? e.target.closest('button,input[type=submit],input[type=button],[role=button]') : null;
    if (!b) return;
    var root = b.form || b.closest('form') || document;
    setTimeout(function () { capture(root); }, 0);
  }, true);
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && e.target && e.target.type === 'password') capture(e.target.form || document);
  }, true);

  // ---------------------------------------------------------------- browser → page
  window.__pmData = function (d) {
    data = Object.assign(data, d || {});
    fillOnLoad();
    // Sign-in forms that appear later (single-page apps).
    if (!window.__pmObserver && data.accounts.length) {
      var t = null;
      window.__pmObserver = new MutationObserver(function () {
        clearTimeout(t); t = setTimeout(fillOnLoad, 250);
      });
      window.__pmObserver.observe(document.documentElement, { childList: true, subtree: true });
      setTimeout(function () { if (window.__pmObserver) window.__pmObserver.disconnect(); }, 20000);
    }
  };
  window.__pmFillCard = function (c) {
    var el = window.__pmCardTarget; if (!el) return;
    var root = el.form || document;
    setValue(el, c.number);
    Array.prototype.forEach.call(root.querySelectorAll('input,select'), function (f) {
      var a = ac(f), h = hint(f);
      if (a.indexOf('cc-name') >= 0 || /name.?on.?card|card.?holder|cc.?name/.test(h)) setValue(f, c.name);
      else if (a.indexOf('cc-exp-month') >= 0 || /exp.*month|cc.?month/.test(h)) setValue(f, ('0' + c.m).slice(-2));
      else if (a.indexOf('cc-exp-year') >= 0 || /exp.*year|cc.?year/.test(h)) setValue(f, String(f.maxLength === 2 ? c.y % 100 : c.y));
      else if (a.indexOf('cc-exp') >= 0 || /expir|exp.?date|mm.?yy/.test(h)) setValue(f, ('0' + c.m).slice(-2) + '/' + String(c.y % 100));
    });
  };
  send({ t: 'ready' });
})();
