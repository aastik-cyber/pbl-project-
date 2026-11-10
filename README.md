# Intelligent Auto Word Suggestion System Using DSA

## Setup
1. Install Python 3 and gcc (Windows: MinGW or WSL), then: pip install -r requirements.txt
2. Compile the C engine (run from this folder):
   Linux/Mac : gcc -Wall engine/engine.c -o engine/engine
   Windows   : gcc engine/engine.c -o engine/engine.exe
3. Run: python backend/app.py
4. Open http://127.0.0.1:5000

## Admin (dictionary manager)
Default password: admin123
Change it in backend/app.py (ADMIN_PASSWORD) or set the ADMIN_PASSWORD environment variable.

## Automatic learning
When you finish a word (space, comma, full stop) the C engine learns it, without asking:
- new word -> added to data/words.txt with frequency 1 (the Trie and heap suggest it from then on)
- known word -> its frequency goes up by 1, so it ranks higher
- the pair (previous word, this word) is saved in data/bigrams.txt for next-word prediction
Wrong words that were learned can be deleted by the admin in the Dictionary manager.
