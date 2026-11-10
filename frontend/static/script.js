/* =====================================================
   script.js - frontend logic (plain JavaScript)
   1. Helper functions    2. On-screen keyboard
   3. Live suggestions    4. Admin login + dictionary
   ===================================================== */

/* ---------- 1. Helper functions ---------- */

// shortcut for document.getElementById
function $(id) {
  return document.getElementById(id);
}

// send a request to Flask and return the JSON answer
async function getJSON(url, options) {
  const response = await fetch(url, options);
  let data;
  try {
    data = await response.json();
  } catch (e) {
    data = { error: "Server problem (code " + response.status + ")" };
  }
  if (data.error) {                         // show the problem instead of failing silently
    showError(data.error);
    if (data.results === undefined) data.results = [];
  } else {
    hideError();
  }
  return data;
}

function showError(message) {
  $("errorBar").textContent = message;
  $("errorBar").classList.remove("hidden");
}

function hideError() {
  $("errorBar").classList.add("hidden");
}

// send JSON data (used for POST requests)
function sendJSON(url, method, data) {
  return getJSON(url, {
    method: method,
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data),
  });
}

// Debounce: wait until the user stops typing for `delay` ms,
// so we do not call the server on every single key press.
function debounce(func, delay) {
  let timer;
  return function () {
    clearTimeout(timer);
    timer = setTimeout(func, delay);
  };
}

// Create one button for every item and put them inside `box`
function showChips(box, items, getText, onClick) {
  box.innerHTML = "";
  items.forEach(function (item) {
    const button = document.createElement("button");
    button.className = "chip";
    button.textContent = getText(item);
    button.addEventListener("click", function () {
      onClick(item);
    });
    box.appendChild(button);
  });
}

/* ---------- 2. On-screen keyboard ---------- */

const editor = $("editor");

// each row of the keyboard; the long names are special keys
const layout = [
  ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
  ["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
  ["a", "s", "d", "f", "g", "h", "j", "k", "l"],
  ["shift", "z", "x", "c", "v", "b", "n", "m", "back"],
  [",", "space", ".", "enter"],
];

const keyLabels = { shift: "Shift", back: "Del", space: "Space", enter: "Enter" };
let shiftOn = false;

function buildKeyboard() {
  const keyboard = $("keyboard");
  keyboard.innerHTML = "";

  layout.forEach(function (row, rowNumber) {
    const rowDiv = document.createElement("div");
    rowDiv.className = "krow";
    if (rowNumber === 2) rowDiv.classList.add("indent");   // the A-row is shorter

    row.forEach(function (key) {
      const button = document.createElement("button");
      button.className = "key";
      button.dataset.key = key;
      button.textContent = keyLabels[key] || key;
      if (keyLabels[key]) button.classList.add("fn");
      if (key === "space") button.classList.add("space");

      // mousedown + preventDefault keeps the cursor inside the text box
      button.addEventListener("mousedown", function (e) { e.preventDefault(); });
      button.addEventListener("click", function () { pressKey(key); });
      rowDiv.appendChild(button);
    });
    keyboard.appendChild(rowDiv);
  });
}

// called when a key on the screen is clicked
function pressKey(key) {
  if (key === "shift") {
    shiftOn = !shiftOn;
    document.querySelector("[data-key='shift']").classList.toggle("shift-on", shiftOn);
    return;
  }

  if (key === "back") {
    editor.value = editor.value.slice(0, -1);
  } else if (key === "space") {
    editor.value += " ";
  } else if (key === "enter") {
    editor.value += "\n";
  } else {
    let letter = key;
    if (shiftOn) {                                   // Shift works for one letter
      letter = key.toUpperCase();
      shiftOn = false;
      document.querySelector("[data-key='shift']").classList.remove("shift-on");
    }
    editor.value += letter;
  }
  editor.focus();
  learnFinishedWord();
  typingHandler();
}

// light up the on-screen key when a real keyboard key is pressed
function findKeyButton(event) {
  let name = event.key.toLowerCase();
  if (name === "backspace") name = "back";
  if (name === " ") name = "space";
  return document.querySelector("[data-key='" + name + "']");
}

document.addEventListener("keydown", function (e) {
  if ($("typingView").classList.contains("hidden")) return;
  const button = findKeyButton(e);
  if (button) button.classList.add("pressed");
});

document.addEventListener("keyup", function (e) {
  const button = findKeyButton(e);
  if (button) button.classList.remove("pressed");
});

/* ---------- 3. Live suggestions, next word and spelling ---------- */

function setStrip(label, items, onClick) {
  $("barLabel").textContent = label;
  showChips($("suggestions"), items,
    function (item) { return item.word; }, onClick);
}

function hideSpell() {
  $("spellBox").classList.add("hidden");
}

function clearAll() {
  setStrip("Suggestions", [], null);
  hideSpell();
}

// ask the C engine if `word` is spelled correctly; show corrections if not
async function showSpelling(word, text) {
  const spell = await getJSON("/api/spell?word=" + word + "&k=4");
  if (editor.value !== text) return;               // user typed more: ignore old answer

  if (spell.correct || spell.results.length === 0) {
    hideSpell();
    return;
  }
  $("spellMsg").textContent = "\"" + word + "\" may be misspelled. Did you mean:";
  showChips($("spellChips"), spell.results,
    function (item) { return item.word; },
    function (item) { fixLastWord(item.word); });

  $("spellBox").classList.remove("hidden");
}

async function handleTyping() {
  const text = editor.value;

  // CASE 1: text ends with a letter -> the user is typing a word
  if (/[a-zA-Z]$/.test(text)) {
    const prefix = text.match(/[a-zA-Z]+$/)[0];
    const data = await getJSON("/api/suggest?prefix=" + prefix + "&k=5");
    if (editor.value !== text) return;

    if (data.results.length > 0) {
      setStrip("Suggestions", data.results, function (item) { pickSuggestion(item.word); });
      hideSpell();
    } else {
      // no word starts with this: maybe a spelling mistake
      setStrip("No word starts with \"" + prefix + "\"", [], null);
      showSpelling(prefix, text);
    }
  }

  // CASE 2: text ends with a space -> the user finished a word
  else if (/\s$/.test(text)) {
    const words = text.toLowerCase().match(/[a-z]+/g);
    if (!words) { clearAll(); return; }
    const lastWord = words[words.length - 1];

    const next = await getJSON("/api/next?word=" + lastWord + "&k=5");
    if (editor.value !== text) return;
    setStrip("Next word", next.results, function (item) { addNextWord(item.word); });

  }

  // CASE 3: empty text or symbols
  else {
    clearAll();
  }
}

// run handleTyping 150 ms after the user stops typing
const typingHandler = debounce(handleTyping, 150);
editor.addEventListener("input", function () {
  learnFinishedWord();          // runs at once (not debounced), so no word is missed
  typingHandler();
});

// replace the half-typed word with the chosen suggestion
function pickSuggestion(word) {
  editor.value = editor.value.replace(/[a-zA-Z]+$/, word + " ");
  editor.focus();
  learnFinishedWord();
  handleTyping();                                  // show next-word predictions now
}

// add the predicted word at the end
function addNextWord(word) {
  editor.value = editor.value + word + " ";
  editor.focus();
  learnFinishedWord();
  handleTyping();
}

// replace the misspelled (last) word with the correction
function fixLastWord(word) {
  editor.value = editor.value.replace(/[a-zA-Z]+\s*$/, word + " ");
  editor.focus();
  learnFinishedWord();
  handleTyping();
}

// ----- automatic learning -----
// A word is "finished" when the text ends with  word + (space, comma, full stop...).
// Then the C engine adds it to the dictionary (if new) or raises its frequency,
// and also learns the pair  previous word -> this word. Nobody is asked anything.
let learnTimer;

function showLearnMessage(message) {
  $("learnMsg").textContent = message;
  clearTimeout(learnTimer);
  learnTimer = setTimeout(function () { $("learnMsg").textContent = ""; }, 3000);
}

function learnFinishedWord() {
  const text = editor.value;
  // (^|[^a-zA-Z']) = start of text or a non-letter, so "don't" is not cut into "t"
  const single = text.match(/(^|[^a-zA-Z'])([a-zA-Z]+)[\s.,!?]$/);
  if (!single) return;                              // no word was just finished
  const pair = text.match(/(^|[^a-zA-Z'])([a-zA-Z]+) +([a-zA-Z]+)[\s.,!?]$/);

  const word = single[2];
  const prev = pair ? pair[2] : "";
  sendJSON("/api/autolearn", "POST", { word: word, prev: prev }).then(function (data) {
    if (data.status === "ADDED") showLearnMessage("Learned new word: \"" + word + "\"");
    if (data.status && data.status.indexOf("ERROR") === 0) showError(data.status);
  });
}

$("clearBtn").addEventListener("click", function () {
  editor.value = "";
  $("learnMsg").textContent = "";
  clearAll();
  editor.focus();
});

/* ---------- 4. Admin login and dictionary ---------- */

let isAdmin = false;

// show either the typing screen or the admin screen
function showView(name) {
  $("typingView").classList.toggle("hidden", name !== "typing");
  $("adminView").classList.toggle("hidden", name !== "admin");
}

function updateAdminButtons() {
  $("adminBtn").title = isAdmin ? "Dictionary manager" : "Admin";
}

// if the server says "401: login required", go back to the keyboard
function handleLoggedOut() {
  isAdmin = false;
  updateAdminButtons();
  showView("typing");
}

function openLogin() {
  $("passwordInput").value = "";
  $("loginError").textContent = "";
  $("loginModal").classList.remove("hidden");
  $("passwordInput").focus();
}

function closeLogin() {
  $("loginModal").classList.add("hidden");
}

$("adminBtn").addEventListener("click", function () {
  if (isAdmin) {
    showView("admin");
    loadDictionary();
  } else {
    openLogin();
  }
});

$("loginCancel").addEventListener("click", closeLogin);

async function doLogin() {
  const data = await sendJSON("/api/admin/login", "POST", { password: $("passwordInput").value });
  if (data.ok) {
    isAdmin = true;
    updateAdminButtons();
    closeLogin();
    showView("admin");
    loadDictionary();
  } else {
    $("loginError").textContent = data.message;
  }
}

$("loginBtn").addEventListener("click", doLogin);
$("passwordInput").addEventListener("keydown", function (e) {
  if (e.key === "Enter") doLogin();
});

$("logoutBtn").addEventListener("click", async function () {
  await sendJSON("/api/admin/logout", "POST", {});
  handleLoggedOut();
});

$("backBtn").addEventListener("click", function () {
  showView("typing");
});

async function loadDictionary() {
  const q = encodeURIComponent($("dictSearch").value.trim());
  const data = await getJSON("/api/dictionary?q=" + q);
  if (data.auth === false) { handleLoggedOut(); return; }

  $("dictCount").textContent = data.results.length + " words shown";
  if (data.results.length === 0 && $("dictSearch").value.trim() === "" && !data.error) {
    $("dictCount").textContent = "The dictionary is empty. Add words or click \"Restore default words\".";
  }
  const body = $("dictBody");
  body.innerHTML = "";

  data.results.forEach(function (item) {
    const row = document.createElement("tr");

    const wordCell = document.createElement("td");
    wordCell.textContent = item.word;

    const freqCell = document.createElement("td");
    freqCell.textContent = item.freq;

    const delCell = document.createElement("td");
    const delBtn = document.createElement("button");
    delBtn.className = "del";
    delBtn.textContent = "Delete";
    delBtn.addEventListener("click", function () { deleteWord(item.word); });
    delCell.appendChild(delBtn);

    row.appendChild(wordCell);
    row.appendChild(freqCell);
    row.appendChild(delCell);
    body.appendChild(row);
  });
}

async function deleteWord(word) {
  if (!confirm("Delete the word \"" + word + "\"?")) return;
  const data = await getJSON("/api/dictionary/" + word, { method: "DELETE" });
  if (data.auth === false) { handleLoggedOut(); return; }
  $("dictMsg").textContent = data.message;
  loadDictionary();
}

$("addBtn").addEventListener("click", async function () {
  const word = $("addWord").value.trim();
  const freq = $("addFreq").value;
  const data = await sendJSON("/api/dictionary", "POST", { word: word, freq: freq });
  if (data.auth === false) { handleLoggedOut(); return; }
  $("dictMsg").textContent = data.message;
  if (data.ok) $("addWord").value = "";
  loadDictionary();
});

$("restoreBtn").addEventListener("click", async function () {
  const data = await sendJSON("/api/dictionary/restore", "POST", {});
  if (data.auth === false) { handleLoggedOut(); return; }
  $("dictMsg").textContent = data.message;
  loadDictionary();
});

$("dictSearch").addEventListener("input", debounce(loadDictionary, 200));

/* ---------- Start ---------- */

buildKeyboard();
getJSON("/api/admin/status").then(function (data) {   // already logged in?
  isAdmin = data.admin;
  updateAdminButtons();
});
