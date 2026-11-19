// Writing Peer Review - Client Application
(function () {
  'use strict';

  // --- THEME / APPEARANCE MANAGEMENT ---
  var THEME_COLOR_MAP = { warm: '#f3e9d6', clean: '#eef2f6', night: '#1e1c1a' };

  function applyTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem('wpr-theme', theme); } catch(e) {}
    var metaTheme = document.querySelector('meta[name="theme-color"]');
    if (metaTheme) metaTheme.setAttribute('content', THEME_COLOR_MAP[theme] || '#f3e9d6');
    updatePickerState();
  }

  function applyAccent(accent) {
    document.documentElement.setAttribute('data-accent', accent);
    try { localStorage.setItem('wpr-accent', accent); } catch(e) {}
    updatePickerState();
  }

  function applyTextSize(size) {
    document.documentElement.setAttribute('data-textsize', size);
    try { localStorage.setItem('wpr-textsize', size); } catch(e) {}
    updatePickerState();
  }

  function updatePickerState() {
    var theme = document.documentElement.getAttribute('data-theme') || 'warm';
    var accent = document.documentElement.getAttribute('data-accent') || 'green';
    var size = document.documentElement.getAttribute('data-textsize') || 'medium';

    document.querySelectorAll('.theme-option').forEach(function(btn) {
      var active = btn.dataset.theme === theme;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
    document.querySelectorAll('.accent-swatch').forEach(function(btn) {
      var active = btn.dataset.accent === accent;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
    document.querySelectorAll('.textsize-option').forEach(function(btn) {
      var active = btn.dataset.textsize === size;
      btn.classList.toggle('active', active);
      btn.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
  }

  // Wire up picker buttons
  document.querySelectorAll('.theme-option').forEach(function(btn) {
    btn.addEventListener('click', function() { applyTheme(btn.dataset.theme); });
  });
  document.querySelectorAll('.accent-swatch').forEach(function(btn) {
    btn.addEventListener('click', function() { applyAccent(btn.dataset.accent); });
  });
  document.querySelectorAll('.textsize-option').forEach(function(btn) {
    btn.addEventListener('click', function() { applyTextSize(btn.dataset.textsize); });
  });

  // Set initial theme-color meta + sync picker on load
  (function initTheme() {
    var theme = document.documentElement.getAttribute('data-theme') || 'warm';
    var metaTheme = document.querySelector('meta[name="theme-color"]');
    if (metaTheme) metaTheme.setAttribute('content', THEME_COLOR_MAP[theme] || '#f3e9d6');
    // Ensure the three attributes are always explicit, so the picker's pressed
    // state can never disagree with the theme tokens in force.
    if (!document.documentElement.hasAttribute('data-theme')) {
      document.documentElement.setAttribute('data-theme', 'warm');
    }
    if (!document.documentElement.hasAttribute('data-accent')) {
      document.documentElement.setAttribute('data-accent', 'green');
    }
    if (!document.documentElement.hasAttribute('data-textsize')) {
      document.documentElement.setAttribute('data-textsize', 'medium');
    }
    updatePickerState();
  })();

  // --- STATE ---
  const state = {
    currentView: 'deal',
    currentPiece: null, // { id, title, body, exercise_id, created_at, updated_at, word_count }
    currentExercise: null,
    exercises: [],
    pieces: [],
    reviews: [],
    portfolioPieces: [], // ordered list of piece objects for portfolio view
    displayName: 'Bethany',
    autosaveTimer: null,
    isSaving: false,
    lastSavedContent: { title: '', body: '' },
  };

  // --- DOM ELEMENTS ---
  const el = {
    // Nav
    navBtns: document.querySelectorAll('.nav-btn'),
    views: document.querySelectorAll('.view-panel'),
    // Header Status Strip
    statusStrip: document.getElementById('status-strip'),
    statusDot: document.getElementById('status-dot'),
    statusLabel: document.getElementById('status-label'),
    statusTime: document.getElementById('status-time'),
    // Deal View
    dealBtn: document.getElementById('deal-btn'),
    freeWriteBtn: document.getElementById('free-write-btn'),
    dealtContainer: document.getElementById('dealt-container'),
    browseToggleBtn: document.getElementById('browse-toggle-btn'),
    browseContainer: document.getElementById('browse-container'),
    browseList: document.getElementById('browse-list'),
    // Write View
    promptBanner: document.getElementById('prompt-banner'),
    promptBannerTitle: document.getElementById('prompt-banner-title'),
    promptBannerText: document.getElementById('prompt-banner-text'),
    closePromptBannerBtn: document.getElementById('close-prompt-banner-btn'),
    titleInput: document.getElementById('piece-title'),
    bodyTextarea: document.getElementById('piece-body'),
    wordCountSpan: document.getElementById('word-count'),
    saveIndicatorSpan: document.getElementById('save-indicator'),
    submitReviewBtn: document.getElementById('submit-review-btn'),
    newPieceBtn: document.getElementById('new-piece-btn'),
    exportPieceMdBtn: document.getElementById('export-piece-md-btn'),
    exportPiecePdfBtn: document.getElementById('export-piece-pdf-btn'),
    exportPieceScope: document.getElementById('export-piece-scope'),
    reviewsSection: document.getElementById('reviews-section'),
    reviewsContainer: document.getElementById('reviews-container'),
    // History View
    historyList: document.getElementById('history-list'),
    // Portfolio View
    portfolioList: document.getElementById('portfolio-list'),
    portfolioIncludeReviews: document.getElementById('portfolio-include-reviews'),
    generatePortfolioBtn: document.getElementById('generate-portfolio-btn'),
    // Settings View
    displayNameInput: document.getElementById('settings-display-name'),
    saveSettingsBtn: document.getElementById('save-settings-btn'),
    settingsFeedback: document.getElementById('settings-feedback'),
  };

  // --- SERVICE WORKER REGISTRATION ---
  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('/sw.js', { scope: '/' })
        .then((reg) => console.log('ServiceWorker registered:', reg.scope))
        .catch((err) => console.log('ServiceWorker registration failed:', err));
    });
  }

  // --- VIEW SWITCHING ---
  function switchView(viewName) {
    state.currentView = viewName;
    el.navBtns.forEach((btn) => {
      btn.classList.toggle('active', btn.dataset.view === viewName);
    });
    el.views.forEach((v) => {
      v.classList.toggle('active', v.id === `view-${viewName}`);
    });

    if (viewName === 'history') {
      loadHistory();
    } else if (viewName === 'portfolio') {
      loadPortfolio();
    } else if (viewName === 'settings') {
      loadSettings();
    }
  }

  el.navBtns.forEach((btn) => {
    btn.addEventListener('click', () => switchView(btn.dataset.view));
  });

  // --- STATUS POLLING ---
  async function refreshStatus() {
    try {
      const res = await fetch('/api/status');
      if (!res.ok) return;
      const data = await res.json();
      state.displayName = data.display_name || 'Bethany';

      const peak = data.peak;
      if (peak) {
        el.statusDot.className = 'status-dot ' + (peak.is_peak ? 'peak' : 'offpeak');
        el.statusLabel.textContent = peak.label || (peak.is_peak ? 'Peak' : 'Off-peak');
        el.statusTime.textContent = `· Flips ${peak.next_flip_local}`;
        el.statusStrip.title = peak.detail || '';
      }
    } catch (e) {
      console.warn('Could not update status:', e);
    }
  }
  refreshStatus();
  setInterval(refreshStatus, 60000); // 60s polling

  // --- DEAL VIEW LOGIC ---
  el.dealBtn.addEventListener('click', async () => {
    el.dealBtn.disabled = true;
    el.dealBtn.textContent = 'Dealing...';
    try {
      const res = await fetch('/api/exercises/deal');
      if (res.ok) {
        const data = await res.json();
        renderDealtExercise(data.exercise);
      }
    } finally {
      el.dealBtn.disabled = false;
      el.dealBtn.textContent = 'Deal me one';
    }
  });

  function renderDealtExercise(exercise) {
    state.currentExercise = exercise;
    el.dealtContainer.innerHTML = `
      <div class="exercise-card">
        <div class="exercise-card-header">
          <span class="badge badge-accent">${escapeHtml(exercise.type)}</span>
          <span class="badge">${escapeHtml(exercise.suggested_length)}</span>
        </div>
        <h3 class="exercise-card-title">${escapeHtml(exercise.title)}</h3>
        <div class="exercise-card-prompt">${escapeHtml(exercise.prompt)}</div>
        <button id="write-to-dealt-btn" class="btn btn-primary">Write to this</button>
      </div>
    `;
    document.getElementById('write-to-dealt-btn').addEventListener('click', () => {
      startNewPiece(exercise);
    });
  }

  el.freeWriteBtn.addEventListener('click', () => {
    startNewPiece(null);
  });

  el.browseToggleBtn.addEventListener('click', async () => {
    const isHidden = el.browseContainer.style.display === 'none' || !el.browseContainer.style.display;
    if (isHidden) {
      el.browseContainer.style.display = 'block';
      el.browseToggleBtn.textContent = 'Hide all exercises';
      if (state.exercises.length === 0) {
        const res = await fetch('/api/exercises');
        if (res.ok) {
          const data = await res.json();
          state.exercises = data.exercises || [];
          renderBrowseList();
        }
      }
    } else {
      el.browseContainer.style.display = 'none';
      el.browseToggleBtn.textContent = 'Browse all exercises';
    }
  });

  function renderBrowseList() {
    const grouped = {};
    state.exercises.forEach((ex) => {
      if (!grouped[ex.type]) grouped[ex.type] = [];
      grouped[ex.type].push(ex);
    });

    let html = '';
    for (const [type, list] of Object.entries(grouped)) {
      html += `<div class="exercises-group"><div class="group-title">${escapeHtml(type)}</div>`;
      list.forEach((ex) => {
        html += `
          <div class="exercise-list-item">
            <div style="display:flex; justify-content:space-between; align-items:center;">
              <span class="exercise-list-title">${escapeHtml(ex.title)}</span>
              <span class="badge">${escapeHtml(ex.suggested_length)}</span>
            </div>
            <p class="exercise-list-prompt">${escapeHtml(ex.prompt)}</p>
            <div>
              <button class="btn btn-subtle browse-pick-btn" data-id="${ex.id}" style="font-size:0.8rem;">Write to this &rarr;</button>
            </div>
          </div>
        `;
      });
      html += `</div>`;
    }
    el.browseList.innerHTML = html;

    el.browseList.querySelectorAll('.browse-pick-btn').forEach((btn) => {
      btn.addEventListener('click', () => {
        const exId = parseInt(btn.dataset.id, 10);
        const chosen = state.exercises.find((e) => e.id === exId);
        if (chosen) startNewPiece(chosen);
      });
    });
  }

  // --- WRITE VIEW LOGIC ---
  function startNewPiece(exercise) {
    state.currentPiece = null;
    state.currentExercise = exercise;
    state.reviews = [];
    state.lastSavedContent = { title: '', body: '' };

    el.titleInput.value = '';
    el.bodyTextarea.value = '';
    updateWordCount();
    el.saveIndicatorSpan.textContent = 'Not yet saved';
    el.submitReviewBtn.disabled = false;
    el.exportPieceMdBtn.disabled = true;
    el.exportPiecePdfBtn.disabled = true;
    el.reviewsContainer.innerHTML = '';
    el.reviewsSection.style.display = 'none';

    if (exercise) {
      el.promptBannerTitle.textContent = `${exercise.type.toUpperCase()}: ${exercise.title}`;
      el.promptBannerText.textContent = exercise.prompt;
      el.promptBanner.style.display = 'flex';
    } else {
      el.promptBanner.style.display = 'none';
    }

    switchView('write');
    el.titleInput.focus();
  }

  el.closePromptBannerBtn.addEventListener('click', () => {
    el.promptBanner.style.display = 'none';
  });

  el.newPieceBtn.addEventListener('click', () => {
    if (confirm('Start a fresh new piece? Any saved work is accessible in History.')) {
      startNewPiece(null);
    }
  });

  function updateWordCount() {
    const text = el.bodyTextarea.value.trim();
    const count = text ? text.split(/\s+/).length : 0;
    el.wordCountSpan.textContent = `${count} word${count === 1 ? '' : 's'}`;
  }

  // Autosave debouncing (~800ms)
  function triggerAutosave() {
    updateWordCount();
    el.saveIndicatorSpan.textContent = 'Unsaved changes...';

    if (state.autosaveTimer) clearTimeout(state.autosaveTimer);
    state.autosaveTimer = setTimeout(performAutosave, 800);
  }

  el.titleInput.addEventListener('input', triggerAutosave);
  el.bodyTextarea.addEventListener('input', triggerAutosave);

  async function performAutosave() {
    const title = el.titleInput.value.trim() || 'Untitled Piece';
    const body = el.bodyTextarea.value;

    if (
      state.currentPiece &&
      title === state.lastSavedContent.title &&
      body === state.lastSavedContent.body
    ) {
      el.saveIndicatorSpan.textContent = 'Saved';
      return;
    }

    // If completely empty and no piece exists, don't create empty row yet
    if (!state.currentPiece && !title && !body.trim()) {
      el.saveIndicatorSpan.textContent = 'Not yet saved';
      return;
    }

    el.saveIndicatorSpan.textContent = 'Saving...';
    state.isSaving = true;

    try {
      if (!state.currentPiece) {
        // Create piece
        const res = await fetch('/api/pieces', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            title: title,
            body: body,
            exercise_id: state.currentExercise ? state.currentExercise.id : null,
          }),
        });
        if (res.ok) {
          const data = await res.json();
          state.currentPiece = data.piece;
          state.lastSavedContent = { title, body };
          const now = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
          el.saveIndicatorSpan.textContent = `Saved at ${now}`;
          el.exportPieceMdBtn.disabled = false;
          el.exportPiecePdfBtn.disabled = false;
        }
      } else {
        // Update piece
        const res = await fetch(`/api/pieces/${state.currentPiece.id}`, {
          method: 'PUT',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ title, body }),
        });
        if (res.ok) {
          const data = await res.json();
          state.currentPiece = data.piece;
          state.lastSavedContent = { title, body };
          const now = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
          el.saveIndicatorSpan.textContent = `Saved at ${now}`;
        }
      }
    } catch (e) {
      el.saveIndicatorSpan.textContent = 'Save error';
      console.error(e);
    } finally {
      state.isSaving = false;
    }
  }

  // Submit for Review
  el.submitReviewBtn.addEventListener('click', async () => {
    // Ensure current content is saved first
    await performAutosave();

    if (!state.currentPiece) {
      alert('Please write something before requesting a peer review.');
      return;
    }

    const originalBtnText = el.submitReviewBtn.textContent;
    el.submitReviewBtn.disabled = true;
    el.submitReviewBtn.textContent = 'Consulting peer review...';

    try {
      const res = await fetch(`/api/pieces/${state.currentPiece.id}/review`, {
        method: 'POST',
      });
      if (res.ok) {
        const data = await res.json();
        state.reviews.push(data.review);
        renderReviews();
        el.reviewsSection.scrollIntoView({ behavior: 'smooth' });
      } else {
        alert('Failed to obtain review.');
      }
    } catch (e) {
      console.error(e);
      alert('Error requesting review.');
    } finally {
      el.submitReviewBtn.disabled = false;
      el.submitReviewBtn.textContent = originalBtnText;
    }
  });

  function renderReviews() {
    if (!state.reviews || state.reviews.length === 0) {
      el.reviewsSection.style.display = 'none';
      return;
    }

    el.reviewsSection.style.display = 'block';
    let html = '';
    // Show newest first
    const sorted = [...state.reviews].reverse();
    sorted.forEach((rev, idx) => {
      const isLatest = idx === 0;
      const prov = escapeHtml(rev.provider || 'peer');
      const dateStr = new Date(rev.created_at).toLocaleString();
      const bodyFormatted = formatReviewMarkdown(rev.body);
      const messagesHtml = renderDiscussionMessages(rev.messages || []);

      html += `
        <div class="review-card ${isLatest ? 'latest' : ''}" data-review-id="${rev.id}">
          <div class="review-card-meta">
            <span class="badge ${prov === 'deepseek' ? 'badge-accent' : ''}">${prov}</span>
            <span>Reviewed on ${dateStr}</span>
          </div>
          <div class="review-content">
            ${bodyFormatted}
          </div>
          <div class="discussion-panel">
            <div class="discussion-header">
              <span class="discussion-title">Discuss this review</span>
              <span class="discussion-hint">Talk back to your peer reviewer about their notes</span>
            </div>
            <div class="discussion-thread" id="discussion-thread-${rev.id}">
              ${messagesHtml}
            </div>
            <div class="discussion-composer">
              <textarea class="discussion-input" id="discussion-input-${rev.id}" placeholder="Ask your peer about this review..." rows="2"></textarea>
              <button class="btn btn-primary discussion-send-btn" data-review-id="${rev.id}">Send</button>
            </div>
          </div>
        </div>
      `;
    });
    el.reviewsContainer.innerHTML = html;

    // Attach discussion send handlers
    el.reviewsContainer.querySelectorAll('.discussion-send-btn').forEach((btn) => {
      btn.addEventListener('click', () => {
        const revId = parseInt(btn.dataset.reviewId, 10);
        handleSendDiscussion(revId);
      });
    });

    // Attach Ctrl/Cmd+Enter on inputs
    el.reviewsContainer.querySelectorAll('.discussion-input').forEach((input) => {
      input.addEventListener('keydown', (e) => {
        if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
          e.preventDefault();
          const revId = parseInt(input.id.replace('discussion-input-', ''), 10);
          handleSendDiscussion(revId);
        }
      });
    });
  }

  function renderDiscussionMessages(messages) {
    if (!messages || messages.length === 0) {
      return '<div class="discussion-empty">No discussion messages yet. Ask your peer reviewer a question about this feedback below.</div>';
    }
    return messages.map((m) => {
      const isUser = (m.role || '').toLowerCase() === 'user';
      const roleName = isUser ? (state.displayName || 'You') : 'Peer Reviewer';
      const timeStr = m.created_at ? new Date(m.created_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
      return `
        <div class="message-bubble ${isUser ? 'user-message' : 'assistant-message'}">
          <div class="message-header">
            <span class="message-sender">${escapeHtml(roleName)}</span>
            ${timeStr ? `<span class="message-time">${escapeHtml(timeStr)}</span>` : ''}
          </div>
          <div class="message-body">${escapeHtml(m.text || '')}</div>
        </div>
      `;
    }).join('');
  }

  async function handleSendDiscussion(reviewId) {
    const inputEl = document.getElementById(`discussion-input-${reviewId}`);
    const sendBtn = el.reviewsContainer.querySelector(`.discussion-send-btn[data-review-id="${reviewId}"]`);
    const threadEl = document.getElementById(`discussion-thread-${reviewId}`);
    if (!inputEl || !sendBtn) return;

    const text = inputEl.value.trim();
    if (!text) return;

    inputEl.disabled = true;
    sendBtn.disabled = true;
    const origBtnText = sendBtn.textContent;
    sendBtn.textContent = 'Sending...';

    try {
      const res = await fetch(`/api/reviews/${reviewId}/messages`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text }),
      });
      if (res.ok) {
        const data = await res.json();
        const rev = state.reviews.find((r) => r.id === reviewId);
        if (rev) {
          if (!rev.messages) rev.messages = [];
          if (data.user_message && data.assistant_message) {
            rev.messages.push(data.user_message);
            rev.messages.push(data.assistant_message);
          } else if (data.messages && data.messages.length >= 2) {
            rev.messages.push(...data.messages);
          }
          if (threadEl) {
            threadEl.innerHTML = renderDiscussionMessages(rev.messages);
            threadEl.scrollTop = threadEl.scrollHeight;
          }
        }
        inputEl.value = '';
      } else {
        alert('Failed to send message.');
      }
    } catch (e) {
      console.error(e);
      alert('Error sending message.');
    } finally {
      inputEl.disabled = false;
      sendBtn.disabled = false;
      sendBtn.textContent = origBtnText;
      inputEl.focus();
    }
  }

  function formatReviewMarkdown(text) {
    if (!text) return '';
    const lines = text.split('\n');
    let out = '';
    let inList = false;
    let currentSection = '';
    let voiceSectionFirstPara = true;
    let inQuote = false;

    lines.forEach((line) => {
      const trimmed = line.trim();
      const isQuoteLine = trimmed.startsWith('> ');
      if (inQuote && !isQuoteLine) {
        out += '</blockquote>';
        inQuote = false;
      }
      if (!trimmed) {
        if (inList) {
          out += '</ul>';
          inList = false;
        }
        return;
      }
      if (trimmed.startsWith('## ')) {
        if (inList) {
          out += '</ul>';
          inList = false;
        }
        const headingText = trimmed.slice(3);
        currentSection = headingText.toLowerCase();
        voiceSectionFirstPara = true;
        out += `<h2>${escapeHtml(headingText)}</h2>`;
      } else if (trimmed.startsWith('### ')) {
        if (inList) {
          out += '</ul>';
          inList = false;
        }
        out += `<h3>${escapeHtml(trimmed.slice(4))}</h3>`;
      } else if (trimmed.startsWith('- ') || trimmed.startsWith('* ')) {
        if (!inList) {
          out += '<ul>';
          inList = true;
        }
        out += `<li>${formatInlineMarkdown(trimmed.slice(2))}</li>`;
      } else if (/^\d+\.\s/.test(trimmed)) {
        if (inList) {
          out += '</ul>';
          inList = false;
        }
        const match = trimmed.match(/^\d+\.\s(.*)$/);
        out += `<p><strong>${escapeHtml(trimmed.split('.')[0])}.</strong> ${formatInlineMarkdown(match ? match[1] : trimmed)}</p>`;
      } else if (isQuoteLine) {
        if (inList) {
          out += '</ul>';
          inList = false;
        }
        if (!inQuote) {
          out += '<blockquote>';
          inQuote = true;
        }
        out += `<p>${formatInlineMarkdown(trimmed.slice(2))}</p>`;
      } else {
        if (inList) {
          out += '</ul>';
          inList = false;
        }
        // Voice check: tag the first paragraph under the voice check section as a verdict band
        if (currentSection.includes('voice') && voiceSectionFirstPara) {
          voiceSectionFirstPara = false;
          const verdictText = trimmed;
          let verdictMod = 'voice-verdict--mixed';
          if (/reads human/i.test(verdictText)) {
            verdictMod = 'voice-verdict--human';
          } else if (/reads machine/i.test(verdictText)) {
            verdictMod = 'voice-verdict--machine';
          }
          out += `<p class="voice-verdict ${verdictMod}">${formatInlineMarkdown(verdictText)}</p>`;
        } else {
          if (currentSection.includes('voice')) voiceSectionFirstPara = false;
          out += `<p>${formatInlineMarkdown(trimmed)}</p>`;
        }
      }
    });

    if (inList) out += '</ul>';
    if (inQuote) out += '</blockquote>';
    return out;
  }

  function formatInlineMarkdown(text) {
    let s = escapeHtml(text);
    // Bold
    s = s.replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>');
    // Italic
    s = s.replace(/\*(.*?)\*/g, '<em>$1</em>');
    // Quotes highlight
    s = s.replace(/&quot;(.*?)&quot;/g, '<q>&quot;$1&quot;</q>');
    s = s.replace(/“(.*?)”/g, '<q>“$1”</q>');
    return s;
  }

  // Single Piece Export
  el.exportPieceMdBtn.addEventListener('click', () => exportSinglePiece('md'));
  el.exportPiecePdfBtn.addEventListener('click', () => exportSinglePiece('pdf'));

  async function exportSinglePiece(format) {
    if (!state.currentPiece) return;
    await performAutosave();

    const scope = el.exportPieceScope ? el.exportPieceScope.value : 'all';
    const includeReviews = scope !== 'piece';
    const includeDiscussions = scope === 'all';

    try {
      const res = await fetch(`/api/export/piece/${state.currentPiece.id}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          format,
          include_reviews: includeReviews,
          include_discussions: includeDiscussions,
        }),
      });
      if (res.ok) {
        const blob = await res.blob();
        downloadBlob(blob, `${state.currentPiece.title || 'piece'}.${format}`);
      } else {
        alert('Export failed.');
      }
    } catch (e) {
      console.error(e);
      alert('Export error.');
    }
  }

  // --- HISTORY VIEW LOGIC ---
  async function loadHistory() {
    el.historyList.innerHTML = '<p style="color:var(--color-ink-mid)">Loading your writing pieces...</p>';
    try {
      const res = await fetch('/api/pieces');
      if (res.ok) {
        const data = await res.json();
        state.pieces = data.pieces || [];
        renderHistory();
      }
    } catch (e) {
      console.error(e);
      el.historyList.innerHTML = '<p style="color:red">Failed to load pieces.</p>';
    }
  }

  function renderHistory() {
    if (state.pieces.length === 0) {
      el.historyList.innerHTML = '<p style="color:var(--color-ink-mid)">No pieces written yet. Go deal yourself a prompt to get started!</p>';
      return;
    }

    let html = '';
    state.pieces.forEach((p) => {
      const updated = new Date(p.updated_at).toLocaleDateString([], { month: 'short', day: 'numeric', year: 'numeric' });
      html += `
        <div class="piece-card" data-id="${p.id}">
          <div class="piece-card-info">
            <h3>${escapeHtml(p.title || 'Untitled Piece')}</h3>
            <div class="piece-card-meta">
              <span>Updated ${updated}</span>
              <span>&bull;</span>
              <span>${p.word_count} words</span>
            </div>
          </div>
          <div class="piece-card-actions">
            <span class="badge ${p.review_count > 0 ? 'badge-accent' : ''}">
              ${p.review_count} ${p.review_count === 1 ? 'review' : 'reviews'}
            </span>
            <button class="btn btn-subtle piece-delete-btn" data-id="${p.id}" title="Delete piece" aria-label="Delete piece">
              Delete
            </button>
          </div>
        </div>
      `;
    });
    el.historyList.innerHTML = html;

    el.historyList.querySelectorAll('.piece-card').forEach((card) => {
      card.addEventListener('click', () => {
        const id = parseInt(card.dataset.id, 10);
        openPieceInEditor(id);
      });
    });

    el.historyList.querySelectorAll('.piece-delete-btn').forEach((btn) => {
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        const id = parseInt(btn.dataset.id, 10);
        handleDeletePiece(id);
      });
    });
  }

  async function handleDeletePiece(pieceId) {
    const piece = state.pieces.find((p) => p.id === pieceId);
    const title = piece ? (piece.title || 'Untitled Piece') : 'Untitled Piece';
    const msg = `Delete '${title}'? Its reviews and discussions go too — this can't be undone.`;
    if (!window.confirm(msg)) {
      return;
    }

    try {
      const res = await fetch(`/api/pieces/${pieceId}`, {
        method: 'DELETE',
      });
      if (res.ok) {
        state.pieces = state.pieces.filter((p) => p.id !== pieceId);
        state.portfolioPieces = state.portfolioPieces.filter((p) => p.id !== pieceId);
        if (state.currentPiece && state.currentPiece.id === pieceId) {
          state.currentPiece = null;
          state.currentExercise = null;
          state.reviews = [];
          state.lastSavedContent = { title: '', body: '' };
          if (state.autosaveTimer) clearTimeout(state.autosaveTimer);
          el.titleInput.value = '';
          el.bodyTextarea.value = '';
          updateWordCount();
          el.saveIndicatorSpan.textContent = 'Not yet saved';
          el.exportPieceMdBtn.disabled = true;
          el.exportPiecePdfBtn.disabled = true;
          el.reviewsContainer.innerHTML = '';
          el.reviewsSection.style.display = 'none';
          el.promptBanner.style.display = 'none';
        }
        renderHistory();
      } else {
        alert('Failed to delete piece.');
      }
    } catch (e) {
      console.error(e);
      alert('Error deleting piece.');
    }
  }

  async function openPieceInEditor(pieceId) {
    try {
      const res = await fetch(`/api/pieces/${pieceId}`);
      if (res.ok) {
        const data = await res.json();
        state.currentPiece = data.piece;
        state.reviews = data.reviews || [];
        state.lastSavedContent = { title: data.piece.title, body: data.piece.body };

        el.titleInput.value = data.piece.title || '';
        el.bodyTextarea.value = data.piece.body || '';
        updateWordCount();
        el.saveIndicatorSpan.textContent = 'Saved';
        el.exportPieceMdBtn.disabled = false;
        el.exportPiecePdfBtn.disabled = false;

        // If linked to an exercise, show banner
        if (data.piece.exercise_id) {
          // Find exercise if already loaded or fetch
          let ex = state.exercises.find((e) => e.id === data.piece.exercise_id);
          if (!ex) {
            const exRes = await fetch('/api/exercises');
            if (exRes.ok) {
              const exData = await exRes.json();
              state.exercises = exData.exercises || [];
              ex = state.exercises.find((e) => e.id === data.piece.exercise_id);
            }
          }
          if (ex) {
            state.currentExercise = ex;
            el.promptBannerTitle.textContent = `${ex.type.toUpperCase()}: ${ex.title}`;
            el.promptBannerText.textContent = ex.prompt;
            el.promptBanner.style.display = 'flex';
          } else {
            el.promptBanner.style.display = 'none';
          }
        } else {
          el.promptBanner.style.display = 'none';
        }

        renderReviews();
        switchView('write');
      }
    } catch (e) {
      console.error(e);
      alert('Could not open piece.');
    }
  }

  // --- PORTFOLIO VIEW LOGIC ---
  async function loadPortfolio() {
    try {
      const res = await fetch('/api/pieces');
      if (res.ok) {
        const data = await res.json();
        // Maintain selection state if pieces already existed
        const existingOrder = state.portfolioPieces.map((p) => p.id);
        const newPieces = data.pieces || [];

        // Order according to existing order if possible
        newPieces.sort((a, b) => {
          const idxA = existingOrder.indexOf(a.id);
          const idxB = existingOrder.indexOf(b.id);
          if (idxA !== -1 && idxB !== -1) return idxA - idxB;
          if (idxA !== -1) return -1;
          if (idxB !== -1) return 1;
          return 0;
        });

        state.portfolioPieces = newPieces.map((p) => {
          const old = state.portfolioPieces.find((o) => o.id === p.id);
          return { ...p, selected: old ? old.selected : true };
        });

        renderPortfolioList();
      }
    } catch (e) {
      console.error(e);
    }
  }

  function renderPortfolioList() {
    if (state.portfolioPieces.length === 0) {
      el.portfolioList.innerHTML = '<p style="color:var(--color-ink-mid)">No pieces to include in a portfolio yet.</p>';
      return;
    }

    let html = '';
    state.portfolioPieces.forEach((p, index) => {
      html += `
        <div class="portfolio-item" data-index="${index}">
          <div class="portfolio-item-left">
            <input type="checkbox" class="portfolio-check" data-id="${p.id}" ${p.selected ? 'checked' : ''}>
            <span style="font-weight:600">${escapeHtml(p.title || 'Untitled')}</span>
            <span style="font-size:0.8rem; color:var(--color-ink-mid)">(${p.word_count} words)</span>
          </div>
          <div class="portfolio-item-reorder">
            <button class="btn btn-subtle move-up-btn" data-index="${index}" ${index === 0 ? 'disabled' : ''}>&uarr;</button>
            <button class="btn btn-subtle move-down-btn" data-index="${index}" ${index === state.portfolioPieces.length - 1 ? 'disabled' : ''}>&darr;</button>
          </div>
        </div>
      `;
    });
    el.portfolioList.innerHTML = html;

    // Checkbox handlers
    el.portfolioList.querySelectorAll('.portfolio-check').forEach((cb) => {
      cb.addEventListener('change', () => {
        const id = parseInt(cb.dataset.id, 10);
        const item = state.portfolioPieces.find((p) => p.id === id);
        if (item) item.selected = cb.checked;
      });
    });

    // Reorder handlers
    el.portfolioList.querySelectorAll('.move-up-btn').forEach((btn) => {
      btn.addEventListener('click', () => {
        const idx = parseInt(btn.dataset.index, 10);
        if (idx > 0) {
          const temp = state.portfolioPieces[idx];
          state.portfolioPieces[idx] = state.portfolioPieces[idx - 1];
          state.portfolioPieces[idx - 1] = temp;
          renderPortfolioList();
        }
      });
    });

    el.portfolioList.querySelectorAll('.move-down-btn').forEach((btn) => {
      btn.addEventListener('click', () => {
        const idx = parseInt(btn.dataset.index, 10);
        if (idx < state.portfolioPieces.length - 1) {
          const temp = state.portfolioPieces[idx];
          state.portfolioPieces[idx] = state.portfolioPieces[idx + 1];
          state.portfolioPieces[idx + 1] = temp;
          renderPortfolioList();
        }
      });
    });
  }

  el.generatePortfolioBtn.addEventListener('click', async () => {
    const selectedIds = state.portfolioPieces
      .filter((p) => p.selected)
      .map((p) => p.id);

    if (selectedIds.length === 0) {
      alert('Please select at least one piece to export.');
      return;
    }

    const format = document.querySelector('input[name="portfolio-format"]:checked').value;
    const includeReviews = el.portfolioIncludeReviews.checked;

    const originalText = el.generatePortfolioBtn.textContent;
    el.generatePortfolioBtn.disabled = true;
    el.generatePortfolioBtn.textContent = 'Generating portfolio...';

    try {
      const res = await fetch('/api/export/portfolio', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          piece_ids: selectedIds,
          format: format,
          include_reviews: includeReviews,
        }),
      });

      if (res.ok) {
        const blob = await res.blob();
        const safeName = (state.displayName || 'portfolio').toLowerCase().replace(/\s+/g, '-');
        downloadBlob(blob, `${safeName}-portfolio.${format}`);
      } else {
        alert('Failed to generate portfolio.');
      }
    } catch (e) {
      console.error(e);
      alert('Error generating portfolio.');
    } finally {
      el.generatePortfolioBtn.disabled = false;
      el.generatePortfolioBtn.textContent = originalText;
    }
  });

  // --- SETTINGS VIEW LOGIC ---
  async function loadSettings() {
    try {
      const res = await fetch('/api/settings');
      if (res.ok) {
        const data = await res.json();
        state.displayName = data.display_name || 'Bethany';
        el.displayNameInput.value = state.displayName;
      }
    } catch (e) {
      console.error(e);
    }
    updatePickerState();
  }

  el.saveSettingsBtn.addEventListener('click', async () => {
    const newName = el.displayNameInput.value.trim() || 'Bethany';
    try {
      const res = await fetch('/api/settings', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ display_name: newName }),
      });
      if (res.ok) {
        const data = await res.json();
        state.displayName = data.display_name;
        el.settingsFeedback.textContent = 'Settings saved.';
        setTimeout(() => {
          el.settingsFeedback.textContent = '';
        }, 3000);
      }
    } catch (e) {
      console.error(e);
      el.settingsFeedback.textContent = 'Save failed.';
    }
  });

  // --- DEVELOP MODE ---

  const devState = {
    pieceId: null,
    threads: [],
    activeThreadId: null,
    messages: [],
    bible: [],
    // Pending merge data
    pendingMerge: null,
    pendingMergeInsertAt: 'end',
  };

  const devEl = {
    room: document.getElementById('develop-room'),
    toggleBtn: document.getElementById('develop-toggle-btn'),
    newThreadBtn: document.getElementById('develop-new-thread-btn'),
    threadTabs: document.getElementById('develop-thread-tabs'),
    transcript: document.getElementById('develop-thread-transcript'),
    threadActions: document.getElementById('develop-thread-actions'),
    saveBibleBtn: document.getElementById('develop-save-bible-btn'),
    addBookBtn: document.getElementById('develop-add-book-btn'),
    proposeCard: document.getElementById('develop-propose-card'),
    proposeEntries: document.getElementById('develop-propose-entries'),
    proposeConfirmBtn: document.getElementById('develop-propose-confirm-btn'),
    proposeCancelBtn: document.getElementById('develop-propose-cancel-btn'),
    mergeCard: document.getElementById('develop-merge-card'),
    mergeBiblePreview: document.getElementById('develop-merge-bible-preview'),
    mergePassagePreview: document.getElementById('develop-merge-passage-preview'),
    mergeWhere: document.getElementById('develop-merge-where'),
    mergePositionSelect: document.getElementById('develop-merge-position-select'),
    mergeApplyBtn: document.getElementById('develop-merge-apply-btn'),
    mergeCancelBtn: document.getElementById('develop-merge-cancel-btn'),
    composer: document.getElementById('develop-composer'),
    input: document.getElementById('develop-input'),
    sendBtn: document.getElementById('develop-send-btn'),
    addBibleBtn: document.getElementById('develop-add-bible-btn'),
    bibleAddForm: document.getElementById('develop-bible-add-form'),
    bibleKindSelect: document.getElementById('develop-bible-kind-select'),
    bibleNameInput: document.getElementById('develop-bible-name-input'),
    bibleDetailInput: document.getElementById('develop-bible-detail-input'),
    bibleSaveBtn: document.getElementById('develop-bible-save-btn'),
    bibleCancelBtn: document.getElementById('develop-bible-cancel-btn'),
    bibleList: document.getElementById('develop-bible-list'),
  };

  let developVisible = false;

  function updateDevelopToggleState() {
    if (devEl.toggleBtn) {
      devEl.toggleBtn.disabled = !state.currentPiece;
    }
    if (el.stuckBtn) {
      el.stuckBtn.disabled = !state.currentPiece;
    }
  }

  // Open/close develop room
  if (devEl.toggleBtn) {
    devEl.toggleBtn.addEventListener('click', () => {
      if (!state.currentPiece) return;
      developVisible = !developVisible;
      devEl.room.style.display = developVisible ? 'block' : 'none';
      devEl.toggleBtn.textContent = developVisible ? 'Close Develop' : 'Develop';
      if (developVisible) {
        devState.pieceId = state.currentPiece.id;
        loadDevelopRoom();
      }
    });
  }

  async function loadDevelopRoom() {
    await Promise.all([loadDevelopThreads(), loadBible()]);
    renderBiblePanel();
    if (devState.threads.length === 0) {
      await createDevelopThread('New conversation');
    } else {
      await selectThread(devState.threads[0].id);
    }
  }

  async function loadDevelopThreads() {
    try {
      const res = await fetch(`/api/pieces/${devState.pieceId}/develop/threads`);
      if (res.ok) {
        const data = await res.json();
        devState.threads = data.threads || [];
      }
    } catch (e) { console.error(e); }
  }

  async function loadBible() {
    try {
      const res = await fetch(`/api/pieces/${devState.pieceId}/bible`);
      if (res.ok) {
        const data = await res.json();
        devState.bible = data.entries || [];
      }
    } catch (e) { console.error(e); }
  }

  async function createDevelopThread(title) {
    try {
      const res = await fetch(`/api/pieces/${devState.pieceId}/develop/threads`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ title: title || 'New conversation' }),
      });
      if (res.ok) {
        const data = await res.json();
        devState.threads.unshift(data.thread);
        renderThreadTabs();
        await selectThread(data.thread.id);
      }
    } catch (e) { console.error(e); }
  }

  if (devEl.newThreadBtn) {
    devEl.newThreadBtn.addEventListener('click', () => createDevelopThread('New conversation'));
  }

  function renderThreadTabs() {
    if (!devEl.threadTabs) return;
    if (devState.threads.length === 0) {
      devEl.threadTabs.innerHTML = '';
      return;
    }
    let html = '';
    devState.threads.forEach((t) => {
      const isActive = t.id === devState.activeThreadId;
      html += `
        <div class="develop-thread-tab ${isActive ? 'active' : ''}" data-thread-id="${t.id}">
          <span>${escapeHtml(t.title)}</span>
          <button class="develop-thread-tab-delete" data-thread-id="${t.id}" title="Delete conversation">×</button>
        </div>
      `;
    });
    devEl.threadTabs.innerHTML = html;

    devEl.threadTabs.querySelectorAll('.develop-thread-tab').forEach((tab) => {
      tab.addEventListener('click', (e) => {
        if (e.target.classList.contains('develop-thread-tab-delete')) return;
        const tid = parseInt(tab.dataset.threadId, 10);
        selectThread(tid);
      });
    });

    devEl.threadTabs.querySelectorAll('.develop-thread-tab-delete').forEach((btn) => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const tid = parseInt(btn.dataset.threadId, 10);
        if (!confirm('Delete this conversation?')) return;
        try {
          const res = await fetch(`/api/develop/threads/${tid}`, { method: 'DELETE' });
          if (res.ok) {
            devState.threads = devState.threads.filter((t) => t.id !== tid);
            if (devState.activeThreadId === tid) {
              devState.activeThreadId = null;
              devState.messages = [];
            }
            renderThreadTabs();
            renderTranscript();
            hideActionCards();
            if (devState.threads.length === 0) {
              devEl.composer.style.display = 'none';
              devEl.threadActions.style.display = 'none';
            } else if (devState.activeThreadId === null) {
              selectThread(devState.threads[0].id);
            }
          }
        } catch (e) { console.error(e); }
      });
    });
  }

  async function selectThread(threadId) {
    devState.activeThreadId = threadId;
    renderThreadTabs();
    hideActionCards();
    try {
      const res = await fetch(`/api/develop/threads/${threadId}`);
      if (res.ok) {
        const data = await res.json();
        devState.messages = data.messages || [];
        renderTranscript();
        devEl.composer.style.display = 'flex';
        // Show action buttons only if there's at least one assistant message
        const hasAssistant = devState.messages.some((m) => m.role === 'assistant');
        devEl.threadActions.style.display = hasAssistant ? 'flex' : 'none';
      }
    } catch (e) { console.error(e); }
  }

  function renderTranscript() {
    if (!devEl.transcript) return;
    if (!devState.activeThreadId) {
      devEl.transcript.innerHTML = '<div class="develop-empty">Select or create a conversation to start.</div>';
      return;
    }
    if (devState.messages.length === 0) {
      devEl.transcript.innerHTML = '<div class="develop-empty">Start the conversation below.</div>';
      return;
    }
    let html = '';
    devState.messages.forEach((m) => {
      const isUser = (m.role || '').toLowerCase() === 'user';
      const roleName = isUser ? (state.displayName || 'You') : 'Development editor';
      const timeStr = m.created_at ? new Date(m.created_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
      html += `
        <div class="message-bubble ${isUser ? 'user-message' : 'assistant-message'}">
          <div class="message-header">
            <span class="message-sender">${escapeHtml(roleName)}</span>
            ${timeStr ? `<span class="message-time">${escapeHtml(timeStr)}</span>` : ''}
          </div>
          <div class="message-body">${escapeHtml(m.text || '')}</div>
        </div>
      `;
    });
    devEl.transcript.innerHTML = html;
    devEl.transcript.scrollTop = devEl.transcript.scrollHeight;
  }

  function hideActionCards() {
    if (devEl.proposeCard) devEl.proposeCard.style.display = 'none';
    if (devEl.mergeCard) devEl.mergeCard.style.display = 'none';
  }

  // Send message
  async function handleDevelopSend() {
    if (!devState.activeThreadId) return;
    const text = devEl.input ? devEl.input.value.trim() : '';
    if (!text) return;

    devEl.input.disabled = true;
    devEl.sendBtn.disabled = true;
    const origText = devEl.sendBtn.textContent;
    devEl.sendBtn.textContent = 'Sending…';
    hideActionCards();

    try {
      const res = await fetch(`/api/develop/threads/${devState.activeThreadId}/messages`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text }),
      });
      if (res.ok) {
        const data = await res.json();
        const newMsgs = data.messages || [];
        devState.messages.push(...newMsgs);
        renderTranscript();
        devEl.input.value = '';
        devEl.threadActions.style.display = 'flex';
        // Update thread updated_at in local list
        const tIdx = devState.threads.findIndex((t) => t.id === devState.activeThreadId);
        if (tIdx !== -1 && newMsgs.length > 0) {
          devState.threads[tIdx].updated_at = newMsgs[newMsgs.length - 1].created_at;
        }
      } else {
        alert('Could not send message.');
      }
    } catch (e) {
      console.error(e);
      alert('Error sending message.');
    } finally {
      devEl.input.disabled = false;
      devEl.sendBtn.disabled = false;
      devEl.sendBtn.textContent = origText;
      if (devEl.input) devEl.input.focus();
    }
  }

  if (devEl.sendBtn) {
    devEl.sendBtn.addEventListener('click', handleDevelopSend);
  }
  if (devEl.input) {
    devEl.input.addEventListener('keydown', (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
        e.preventDefault();
        handleDevelopSend();
      }
    });
  }

  // --- Save to story bible (propose) ---
  if (devEl.saveBibleBtn) {
    devEl.saveBibleBtn.addEventListener('click', async () => {
      if (!devState.activeThreadId) return;
      hideActionCards();
      devEl.saveBibleBtn.disabled = true;
      devEl.saveBibleBtn.textContent = 'Looking through the conversation…';
      try {
        const res = await fetch(`/api/develop/threads/${devState.activeThreadId}/propose-bible`, {
          method: 'POST',
        });
        if (res.ok) {
          const data = await res.json();
          const entries = data.entries || [];
          if (entries.length === 0) {
            devEl.proposeEntries.innerHTML = '<p style="font-family:var(--font-sans);font-size:var(--text-sm);color:var(--color-ink-mid);">Nothing clearly settled in this conversation yet — keep talking!</p>';
          } else {
            let html = '';
            entries.forEach((entry, idx) => {
              html += `
                <div class="develop-propose-entry">
                  <label>
                    <input type="checkbox" class="propose-entry-cb" data-idx="${idx}" checked>
                    <span class="entry-kind">${escapeHtml(entry.kind)}</span>
                    <span>
                      ${entry.name ? `<span class="entry-name">${escapeHtml(entry.name)}</span> — ` : ''}
                      <span class="entry-detail">${escapeHtml(entry.detail)}</span>
                    </span>
                  </label>
                </div>
              `;
            });
            devEl.proposeEntries.innerHTML = html;
            devEl.proposeEntries.dataset.entries = JSON.stringify(entries);
          }
          devEl.proposeCard.style.display = 'block';
        } else {
          alert('Could not get proposals.');
        }
      } catch (e) {
        console.error(e);
        alert('Error getting proposals.');
      } finally {
        devEl.saveBibleBtn.disabled = false;
        devEl.saveBibleBtn.textContent = 'Save to story bible';
      }
    });
  }

  if (devEl.proposeCancelBtn) {
    devEl.proposeCancelBtn.addEventListener('click', () => {
      devEl.proposeCard.style.display = 'none';
    });
  }

  if (devEl.proposeConfirmBtn) {
    devEl.proposeConfirmBtn.addEventListener('click', async () => {
      let entries = [];
      try {
        entries = JSON.parse(devEl.proposeEntries.dataset.entries || '[]');
      } catch (e) { entries = []; }

      const checked = Array.from(devEl.proposeEntries.querySelectorAll('.propose-entry-cb'))
        .filter((cb) => cb.checked)
        .map((cb) => parseInt(cb.dataset.idx, 10));
      const toSave = entries.filter((_, idx) => checked.includes(idx));

      if (toSave.length === 0) {
        devEl.proposeCard.style.display = 'none';
        return;
      }

      devEl.proposeConfirmBtn.disabled = true;
      devEl.proposeConfirmBtn.textContent = 'Saving…';

      try {
        for (const entry of toSave) {
          const res = await fetch(`/api/pieces/${devState.pieceId}/bible`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(entry),
          });
          if (res.ok) {
            const data = await res.json();
            devState.bible.push(data.entry);
          }
        }
        devEl.proposeCard.style.display = 'none';
        renderBiblePanel();
      } catch (e) {
        console.error(e);
        alert('Error saving entries.');
      } finally {
        devEl.proposeConfirmBtn.disabled = false;
        devEl.proposeConfirmBtn.textContent = 'Confirm';
      }
    });
  }

  // --- Add to my book (merge) ---
  if (devEl.addBookBtn) {
    devEl.addBookBtn.addEventListener('click', async () => {
      if (!devState.activeThreadId) return;
      hideActionCards();
      devEl.addBookBtn.disabled = true;
      devEl.addBookBtn.textContent = 'Preparing preview…';
      try {
        const res = await fetch(`/api/develop/threads/${devState.activeThreadId}/merge-preview`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ insert_after_paragraph: 'end' }),
        });
        if (res.ok) {
          const data = await res.json();
          devState.pendingMerge = data;
          devState.pendingMergeInsertAt = 'end';
          renderMergeCard(data);
          devEl.mergeCard.style.display = 'block';
        } else {
          alert('Could not prepare preview.');
        }
      } catch (e) {
        console.error(e);
        alert('Error preparing preview.');
      } finally {
        devEl.addBookBtn.disabled = false;
        devEl.addBookBtn.textContent = 'Add to my book';
      }
    });
  }

  function renderMergeCard(data) {
    const entries = data.bible_entries || [];
    const passage = data.passage || '';

    // Bible entries preview
    if (entries.length > 0) {
      let html = '<div style="font-family:var(--font-sans);font-size:var(--text-xs);font-weight:700;color:var(--color-ink-mid);margin-bottom:var(--sp-2);">Story bible entries to add:</div>';
      entries.forEach((e) => {
        html += `<div class="develop-propose-entry"><span class="entry-kind">${escapeHtml(e.kind)}</span> <span class="entry-name">${escapeHtml(e.name)}</span> — <span class="entry-detail">${escapeHtml(e.detail)}</span></div>`;
      });
      devEl.mergeBiblePreview.innerHTML = html;
      devEl.mergeBiblePreview.style.display = 'block';
    } else {
      devEl.mergeBiblePreview.innerHTML = '';
      devEl.mergeBiblePreview.style.display = 'none';
    }

    // Passage preview
    if (passage.trim()) {
      devEl.mergePassagePreview.textContent = passage;
      devEl.mergePassagePreview.style.display = 'block';

      // Build position options
      const body = el.bodyTextarea ? el.bodyTextarea.value : '';
      const paragraphs = body ? body.split(/\n{2,}/) : [];
      let optHtml = '<option value="end">At the end</option>';
      paragraphs.forEach((p, idx) => {
        const preview = p.trim().slice(0, 40) + (p.trim().length > 40 ? '…' : '');
        optHtml += `<option value="${idx}">After paragraph ${idx + 1}: "${escapeHtml(preview)}"</option>`;
      });
      devEl.mergePositionSelect.innerHTML = optHtml;
      devEl.mergeWhere.style.display = 'flex';
    } else {
      devEl.mergePassagePreview.style.display = 'none';
      devEl.mergeWhere.style.display = 'none';
    }
  }

  if (devEl.mergeCancelBtn) {
    devEl.mergeCancelBtn.addEventListener('click', () => {
      devEl.mergeCard.style.display = 'none';
      devState.pendingMerge = null;
    });
  }

  if (devEl.mergeApplyBtn) {
    devEl.mergeApplyBtn.addEventListener('click', async () => {
      if (!devState.pendingMerge || !devState.activeThreadId) return;
      const data = devState.pendingMerge;
      let insertAt = devEl.mergePositionSelect.value === 'end' ? 'end' : parseInt(devEl.mergePositionSelect.value, 10);

      devEl.mergeApplyBtn.disabled = true;
      devEl.mergeApplyBtn.textContent = 'Applying…';

      try {
        const res = await fetch(`/api/develop/threads/${devState.activeThreadId}/merge-apply`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            bible_entries: data.bible_entries || [],
            passage: data.passage || '',
            insert_after_paragraph: insertAt,
            confirmed: true,
          }),
        });
        if (res.ok) {
          const result = await res.json();
          // Update piece body in state and editor
          if (result.piece) {
            state.currentPiece = result.piece;
            if (el.bodyTextarea) el.bodyTextarea.value = result.piece.body || '';
            updateWordCount();
            state.lastSavedContent = {
              title: state.currentPiece.title,
              body: state.currentPiece.body,
            };
            const now = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
            if (el.saveIndicatorSpan) el.saveIndicatorSpan.textContent = `Saved at ${now}`;
          }
          // Update bible
          if (result.applied && result.applied.bible_entries) {
            devState.bible.push(...result.applied.bible_entries);
            renderBiblePanel();
          }
          devEl.mergeCard.style.display = 'none';
          devState.pendingMerge = null;
        } else {
          alert('Could not apply changes.');
        }
      } catch (e) {
        console.error(e);
        alert('Error applying changes.');
      } finally {
        devEl.mergeApplyBtn.disabled = false;
        devEl.mergeApplyBtn.textContent = 'Add it';
      }
    });
  }

  // --- Bible panel ---
  function renderBiblePanel() {
    if (!devEl.bibleList) return;
    if (devState.bible.length === 0) {
      devEl.bibleList.innerHTML = '<div style="font-family:var(--font-sans);font-size:var(--text-xs);color:var(--color-ink-dim);font-style:italic;">No entries yet.</div>';
      return;
    }
    const groups = {};
    devState.bible.forEach((e) => {
      if (!groups[e.kind]) groups[e.kind] = [];
      groups[e.kind].push(e);
    });
    let html = '';
    for (const [kind, entries] of Object.entries(groups)) {
      html += `<div class="develop-bible-group-title">${escapeHtml(kind)}</div>`;
      entries.forEach((e) => {
        html += `
          <div class="develop-bible-entry" data-entry-id="${e.id}">
            ${e.name ? `<span class="develop-bible-entry-name">${escapeHtml(e.name)}</span>` : ''}
            <span class="develop-bible-entry-detail">${escapeHtml(e.detail)}</span>
            <div class="develop-bible-entry-actions">
              <button class="edit-bible-btn" data-id="${e.id}" title="Edit">Edit</button>
              <button class="del-btn delete-bible-btn" data-id="${e.id}" title="Delete">Delete</button>
            </div>
          </div>
        `;
      });
    }
    devEl.bibleList.innerHTML = html;

    devEl.bibleList.querySelectorAll('.delete-bible-btn').forEach((btn) => {
      btn.addEventListener('click', async () => {
        const entryId = parseInt(btn.dataset.id, 10);
        if (!confirm('Delete this bible entry?')) return;
        try {
          const res = await fetch(`/api/bible/${entryId}`, { method: 'DELETE' });
          if (res.ok) {
            devState.bible = devState.bible.filter((e) => e.id !== entryId);
            renderBiblePanel();
          }
        } catch (e) { console.error(e); }
      });
    });

    devEl.bibleList.querySelectorAll('.edit-bible-btn').forEach((btn) => {
      btn.addEventListener('click', () => {
        const entryId = parseInt(btn.dataset.id, 10);
        const entry = devState.bible.find((e) => e.id === entryId);
        if (!entry) return;
        // Show add form pre-filled for editing
        devEl.bibleKindSelect.value = entry.kind;
        devEl.bibleNameInput.value = entry.name;
        devEl.bibleDetailInput.value = entry.detail;
        devEl.bibleAddForm.style.display = 'block';
        devEl.bibleAddForm.dataset.editId = entryId;
        devEl.bibleNameInput.focus();
      });
    });
  }

  if (devEl.addBibleBtn) {
    devEl.addBibleBtn.addEventListener('click', () => {
      devEl.bibleAddForm.style.display = devEl.bibleAddForm.style.display === 'none' ? 'block' : 'none';
      devEl.bibleAddForm.dataset.editId = '';
      devEl.bibleKindSelect.value = 'character';
      devEl.bibleNameInput.value = '';
      devEl.bibleDetailInput.value = '';
      if (devEl.bibleAddForm.style.display === 'block') devEl.bibleNameInput.focus();
    });
  }

  if (devEl.bibleCancelBtn) {
    devEl.bibleCancelBtn.addEventListener('click', () => {
      devEl.bibleAddForm.style.display = 'none';
      devEl.bibleAddForm.dataset.editId = '';
    });
  }

  if (devEl.bibleSaveBtn) {
    devEl.bibleSaveBtn.addEventListener('click', async () => {
      const kind = devEl.bibleKindSelect.value;
      const name = devEl.bibleNameInput.value.trim();
      const detail = devEl.bibleDetailInput.value.trim();
      if (!detail) { alert('Please add a detail.'); return; }

      const editId = devEl.bibleAddForm.dataset.editId ? parseInt(devEl.bibleAddForm.dataset.editId, 10) : null;

      devEl.bibleSaveBtn.disabled = true;
      devEl.bibleSaveBtn.textContent = 'Saving…';

      try {
        if (editId) {
          const res = await fetch(`/api/bible/${editId}`, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ kind, name, detail }),
          });
          if (res.ok) {
            const data = await res.json();
            const idx = devState.bible.findIndex((e) => e.id === editId);
            if (idx !== -1) devState.bible[idx] = data.entry;
          }
        } else {
          const res = await fetch(`/api/pieces/${devState.pieceId}/bible`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ kind, name, detail }),
          });
          if (res.ok) {
            const data = await res.json();
            devState.bible.push(data.entry);
          }
        }
        devEl.bibleAddForm.style.display = 'none';
        devEl.bibleAddForm.dataset.editId = '';
        renderBiblePanel();
      } catch (e) {
        console.error(e);
        alert('Error saving bible entry.');
      } finally {
        devEl.bibleSaveBtn.disabled = false;
        devEl.bibleSaveBtn.textContent = 'Save';
      }
    });
  }

  // Extend startNewPiece/openPieceInEditor to reset develop room
  const _origStartNewPiece = startNewPiece;
  // Wrap openPieceInEditor to also reset develop state
  const _origOpenPiece = openPieceInEditor;

  function resetDevelopRoom() {
    developVisible = false;
    if (devEl.room) devEl.room.style.display = 'none';
    if (devEl.toggleBtn) {
      devEl.toggleBtn.textContent = 'Develop';
      devEl.toggleBtn.disabled = true;
    }
    if (el.stuckBtn) el.stuckBtn.disabled = true;
    devState.pieceId = null;
    devState.threads = [];
    devState.activeThreadId = null;
    devState.messages = [];
    devState.bible = [];
    devState.pendingMerge = null;
    hideActionCards();
    if (devEl.threadTabs) devEl.threadTabs.innerHTML = '';
    if (devEl.transcript) devEl.transcript.innerHTML = '';
    if (devEl.bibleList) devEl.bibleList.innerHTML = '';
    if (devEl.composer) devEl.composer.style.display = 'none';
    if (devEl.threadActions) devEl.threadActions.style.display = 'none';
  }

  // Monkey-patch startNewPiece to reset develop state
  startNewPiece = function(exercise) {
    resetDevelopRoom();
    _origStartNewPiece(exercise);
  };

  openPieceInEditor = function(pieceId) {
    resetDevelopRoom();
    return _origOpenPiece(pieceId).then ? _origOpenPiece(pieceId).then(() => {
      updateDevelopToggleState();
    }) : _origOpenPiece(pieceId);
  };

  // After pieces load / save, also enable develop toggle
  const origPerformAutosave = performAutosave;
  performAutosave = async function() {
    await origPerformAutosave.apply(this, arguments);
    updateDevelopToggleState();
  };

  // --- STUCK? ---
  el.stuckBtn = document.getElementById('stuck-btn');
  const stuckCard = document.getElementById('stuck-card');
  const stuckDirectionsEl = document.getElementById('stuck-directions');
  const stuckDismissBtn = document.getElementById('stuck-dismiss-btn');

  if (el.stuckBtn) {
    el.stuckBtn.addEventListener('click', async () => {
      if (!state.currentPiece) return;
      el.stuckBtn.disabled = true;
      el.stuckBtn.textContent = 'Thinking…';
      if (stuckCard) stuckCard.style.display = 'none';

      try {
        const res = await fetch(`/api/pieces/${state.currentPiece.id}/stuck`, {
          method: 'POST',
        });
        if (res.ok) {
          const data = await res.json();
          if (stuckDirectionsEl) stuckDirectionsEl.textContent = data.directions || '';
          if (stuckCard) stuckCard.style.display = 'block';
        } else if (res.status === 429) {
          const data = await res.json();
          const msg = (data.detail && data.detail.detail) ? data.detail.detail : (data.detail || "that's your three for this hour, write a little and it comes back");
          if (stuckDirectionsEl) stuckDirectionsEl.textContent = msg;
          if (stuckCard) stuckCard.style.display = 'block';
        } else {
          alert('Could not get directions.');
        }
      } catch (e) {
        console.error(e);
        alert('Error.');
      } finally {
        el.stuckBtn.disabled = !state.currentPiece;
        el.stuckBtn.textContent = 'Stuck?';
      }
    });
  }

  if (stuckDismissBtn && stuckCard) {
    stuckDismissBtn.addEventListener('click', () => {
      stuckCard.style.display = 'none';
    });
  }

  // --- UTILS ---
  function escapeHtml(str) {
    if (!str) return '';
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
  }

  function downloadBlob(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }

})();
