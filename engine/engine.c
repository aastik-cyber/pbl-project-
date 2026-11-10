/*
 * engine.c  -  All DSA logic of the project (written in basic C)
 *
 * Usage (called by Flask):
 *   engine suggest <prefix> <k>      -> top k words starting with prefix
 *   engine spell   <word>  <k>       -> EXACT / WRONG + corrections
 *   engine next    <word>  <k>       -> top k next-word predictions
 *   engine autolearn <word> <prev|-> -> learn a typed word + word pair
 *   engine list    <filter>          -> all dictionary words (A-Z)
 *   engine add     <word> <freq>     -> add a word to the file
 *   engine delete  <word>            -> remove a word from the file
 *
 * Sections:  1. Trie   2. Max Heap   3. Edit Distance   4. Bigrams   5. Files
 *            6. Auto learning
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_WORD   50
#define MAX_ITEMS  20000
#define MAX_PAIRS  5000
#define WORDS_FILE  "data/words.txt"
#define BIGRAM_FILE "data/bigrams.txt"
#define TEMP_FILE   "data/temp.txt"

/* ================= 1. TRIE ================= */
typedef struct TrieNode {
    struct TrieNode *child[26];   /* one pointer for each letter a-z */
    int isEnd;                    /* 1 if a word ends here           */
    int freq;                     /* how often the word is used      */
} TrieNode;

TrieNode *root;

TrieNode *newNode() {
    return (TrieNode *)calloc(1, sizeof(TrieNode));   /* calloc sets all to 0/NULL */
}

void insertWord(const char *word, int freq) {
    TrieNode *cur = root;
    for (int i = 0; word[i] != '\0'; i++) {
        int idx = word[i] - 'a';                      /* 'a'->0, 'b'->1 ... */
        if (cur->child[idx] == NULL)
            cur->child[idx] = newNode();
        cur = cur->child[idx];
    }
    cur->isEnd = 1;
    cur->freq = freq;
}

/* walk down the trie following the letters; NULL if the path does not exist */
TrieNode *findNode(const char *prefix) {
    TrieNode *cur = root;
    for (int i = 0; prefix[i] != '\0'; i++) {
        int idx = prefix[i] - 'a';
        if (cur->child[idx] == NULL) return NULL;
        cur = cur->child[idx];
    }
    return cur;
}

/* ================= 2. MAX HEAP ================= */
typedef struct {
    char word[MAX_WORD];
    int freq;
    int dist;      /* edit distance (0 for normal suggestions) */
} Item;

Item heap[MAX_ITEMS];
int heapSize = 0;

/* "a is better than b" -> smaller distance first, then higher frequency */
int better(Item a, Item b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.freq > b.freq;
}

void swapItems(int i, int j) {
    Item t = heap[i];
    heap[i] = heap[j];
    heap[j] = t;
}

void heapPush(const char *word, int freq, int dist) {
    if (heapSize >= MAX_ITEMS) return;
    int i = heapSize++;
    strcpy(heap[i].word, word);
    heap[i].freq = freq;
    heap[i].dist = dist;
    /* move up while better than parent */
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (better(heap[i], heap[parent])) { swapItems(i, parent); i = parent; }
        else break;
    }
}

Item heapPop() {
    Item top = heap[0];
    heap[0] = heap[--heapSize];
    int i = 0;
    /* move down while a child is better */
    while (1) {
        int l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < heapSize && better(heap[l], heap[best])) best = l;
        if (r < heapSize && better(heap[r], heap[best])) best = r;
        if (best == i) break;
        swapItems(i, best);
        i = best;
    }
    return top;
}

void printTop(int k) {
    while (k > 0 && heapSize > 0) {
        Item it = heapPop();
        printf("%s %d %d\n", it.word, it.freq, it.dist);
        k--;
    }
}

/* ================= 3. EDIT DISTANCE ================= */
int min3(int a, int b, int c) {
    int m = a;
    if (b < m) m = b;
    if (c < m) m = c;
    return m;
}

int editDistance(const char *a, const char *b) {
    int n = strlen(a), m = strlen(b);
    int dp[MAX_WORD + 1][MAX_WORD + 1];
    for (int i = 0; i <= n; i++) dp[i][0] = i;     /* delete all letters */
    for (int j = 0; j <= m; j++) dp[0][j] = j;     /* insert all letters */
    for (int i = 1; i <= n; i++) {
        for (int j = 1; j <= m; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = min3(dp[i - 1][j] + 1,          /* delete  */
                            dp[i][j - 1] + 1,          /* insert  */
                            dp[i - 1][j - 1] + cost);  /* replace */
        }
    }
    return dp[n][m];
}

/* ===== visit every word below a trie node (DFS) =====
   mode 0: push into heap (suggestions)
   mode 1: push into heap if edit distance to `target` <= 2 (spelling)
   mode 2: print directly if word contains `target` (dictionary list) */
char target[MAX_WORD];

void collect(TrieNode *node, char *buf, int len, int mode) {
    if (node->isEnd) {
        buf[len] = '\0';
        if (mode == 0) {
            heapPush(buf, node->freq, 0);
        } else if (mode == 1) {
            int d = editDistance(buf, target);
            if (d <= 2) heapPush(buf, node->freq, d);
        } else {
            if (strstr(buf, target) != NULL) printf("%s %d\n", buf, node->freq);
        }
    }
    for (int i = 0; i < 26; i++) {
        if (node->child[i] != NULL) {
            buf[len] = 'a' + i;
            collect(node->child[i], buf, len + 1, mode);
        }
    }
}

/* ================= 4. BIGRAMS (word pairs) ================= */
char firstW[MAX_PAIRS][MAX_WORD];
char secondW[MAX_PAIRS][MAX_WORD];
int pairCount[MAX_PAIRS];
int numPairs = 0;

void loadBigrams() {
    FILE *f = fopen(BIGRAM_FILE, "r");
    if (f == NULL) return;
    while (numPairs < MAX_PAIRS &&
           fscanf(f, "%49s %49s %d", firstW[numPairs], secondW[numPairs], &pairCount[numPairs]) == 3)
        numPairs++;
    fclose(f);
}

void saveBigrams() {
    FILE *f = fopen(BIGRAM_FILE, "w");
    if (f == NULL) return;
    for (int i = 0; i < numPairs; i++)
        fprintf(f, "%s %s %d\n", firstW[i], secondW[i], pairCount[i]);
    fclose(f);
}

void addPair(const char *a, const char *b) {
    for (int i = 0; i < numPairs; i++) {
        if (strcmp(firstW[i], a) == 0 && strcmp(secondW[i], b) == 0) {
            pairCount[i]++;                 /* pair already known: count +1 */
            return;
        }
    }
    if (numPairs < MAX_PAIRS) {             /* new pair */
        strcpy(firstW[numPairs], a);
        strcpy(secondW[numPairs], b);
        pairCount[numPairs] = 1;
        numPairs++;
    }
}

/* ================= 5. FILE HANDLING ================= */
int isValid(const char *w) {
    int len = strlen(w);
    if (len < 1 || len > 40) return 0;
    for (int i = 0; i < len; i++)
        if (w[i] < 'a' || w[i] > 'z') return 0;
    return 1;
}

void toLower(char *w) {
    for (int i = 0; w[i] != '\0'; i++) w[i] = tolower((unsigned char)w[i]);
}

/* read words.txt line by line: one bad line can never stop the loading */
void loadWords() {
    FILE *f = fopen(WORDS_FILE, "r");
    if (f == NULL) return;
    char line[200], w[MAX_WORD];
    while (fgets(line, sizeof(line), f)) {
        int freq = 1;                               /* default if the number is missing */
        if (sscanf(line, "%49s %d", w, &freq) >= 1) {
            toLower(w);
            if (isValid(w)) insertWord(w, freq);
        }
    }
    fclose(f);
}

/* 1 if the file is empty or its last character is a newline */
int endsWithNewline(const char *name) {
    FILE *f = fopen(name, "rb");
    if (f == NULL) return 1;
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) == 0) { fclose(f); return 1; }
    fseek(f, -1, SEEK_END);
    int c = fgetc(f);
    fclose(f);
    return c == '\n';
}

/* add "word freq" at the end of words.txt; returns 0 if the file cannot be written */
int appendWord(const char *word, int freq) {
    int needNewline = !endsWithNewline(WORDS_FILE);  /* last line had no newline */
    FILE *f = fopen(WORDS_FILE, "a");
    if (f == NULL) return 0;
    if (needNewline) fputc('\n', f);
    fprintf(f, "%s %d\n", word, freq);
    fclose(f);
    return 1;
}

/* replace `target` by the temp file; if rename fails, copy it so no data is lost */
int replaceFile(const char *tempName, const char *target) {
    remove(target);
    if (rename(tempName, target) == 0) return 1;
    FILE *in = fopen(tempName, "r");
    FILE *out = fopen(target, "w");
    if (in == NULL || out == NULL) {
        if (in != NULL) fclose(in);
        if (out != NULL) fclose(out);
        return 0;
    }
    int ch;
    while ((ch = fgetc(in)) != EOF) fputc(ch, out);
    fclose(in);
    fclose(out);
    remove(tempName);
    return 1;
}

/* rewrite words.txt through a temp file.
   mode 0: add 1 to the frequency of `word`      mode 1: delete `word`
   all other lines are copied unchanged */
int rewriteWordsFile(const char *word, int mode) {
    FILE *in = fopen(WORDS_FILE, "r");
    if (in == NULL) return 0;
    FILE *out = fopen(TEMP_FILE, "w");
    if (out == NULL) { fclose(in); return 0; }

    char line[200], w[MAX_WORD];
    while (fgets(line, sizeof(line), in)) {
        int freq = 1;
        if (sscanf(line, "%49s %d", w, &freq) >= 1) {
            toLower(w);
            if (strcmp(w, word) == 0) {
                if (mode == 1) continue;              /* delete: skip this line */
                fprintf(out, "%s %d\n", w, freq + 1); /* bump: frequency + 1    */
                continue;
            }
        }
        fputs(line, out);
        if (strchr(line, '\n') == NULL) fputc('\n', out);
    }
    fclose(in);
    fclose(out);
    return replaceFile(TEMP_FILE, WORDS_FILE);
}

/* ================= 6. AUTO LEARNING =================
   Called every time the user finishes typing a word.
   - word already in the dictionary -> its frequency goes up by 1
   - new word -> it is added to the dictionary file with frequency 1
   - the pair (previous word, this word) is learned for next-word prediction */

void autoLearn(const char *word, const char *prev) {
    TrieNode *node = findNode(word);

    if (node != NULL && node->isEnd) {
        if (!rewriteWordsFile(word, 0)) { printf("ERROR Cannot write data/words.txt\n"); return; }
        printf("UPDATED\n");                   /* known word: frequency + 1 */
    } else {
        if (!appendWord(word, 1)) { printf("ERROR Cannot write data/words.txt\n"); return; }
        printf("ADDED\n");                     /* new word: appended to the file */
    }

    /* learn the word pair  previous word -> this word */
    if (strcmp(prev, "-") != 0 && isValid(prev)) {
        TrieNode *p = findNode(prev);
        if (p != NULL && p->isEnd) {
            addPair(prev, word);
            saveBigrams();
        }
    }
}

/* ================= MAIN ================= */
int main(int argc, char *argv[]) {
    if (argc < 2) { printf("ERROR no command\n"); return 1; }

    root = newNode();
    loadWords();
    loadBigrams();

    char cmd[20];
    strncpy(cmd, argv[1], 19);
    cmd[19] = '\0';
    char buf[MAX_WORD];

    if (strcmp(cmd, "suggest") == 0 && argc >= 4) {
        toLower(argv[2]);
        if (!isValid(argv[2])) return 0;
        TrieNode *node = findNode(argv[2]);
        if (node != NULL) {
            strcpy(buf, argv[2]);
            collect(node, buf, strlen(buf), 0);
            printTop(atoi(argv[3]));
        }
    }
    else if (strcmp(cmd, "spell") == 0 && argc >= 4) {
        toLower(argv[2]);
        if (!isValid(argv[2])) { printf("WRONG\n"); return 0; }
        TrieNode *node = findNode(argv[2]);
        if (node != NULL && node->isEnd) {
            printf("EXACT\n");
        } else {
            printf("WRONG\n");
            strcpy(target, argv[2]);
            buf[0] = '\0';
            collect(root, buf, 0, 1);
            printTop(atoi(argv[3]));
        }
    }
    else if (strcmp(cmd, "next") == 0 && argc >= 4) {
        toLower(argv[2]);
        for (int i = 0; i < numPairs; i++)
            if (strcmp(firstW[i], argv[2]) == 0)
                heapPush(secondW[i], pairCount[i], 0);
        printTop(atoi(argv[3]));
    }
    else if (strcmp(cmd, "autolearn") == 0 && argc >= 4) {
        toLower(argv[2]);
        toLower(argv[3]);
        if (!isValid(argv[2])) { printf("IGNORED\n"); return 0; }
        autoLearn(argv[2], argv[3]);
    }
    else if (strcmp(cmd, "list") == 0) {
        target[0] = '\0';
        if (argc >= 3) { toLower(argv[2]); strncpy(target, argv[2], MAX_WORD - 1); }
        buf[0] = '\0';
        collect(root, buf, 0, 2);
    }
    else if (strcmp(cmd, "add") == 0 && argc >= 4) {
        toLower(argv[2]);
        if (!isValid(argv[2])) { printf("ERROR Use only letters a-z\n"); return 0; }
        TrieNode *node = findNode(argv[2]);
        if (node != NULL && node->isEnd) { printf("ERROR Word already exists\n"); return 0; }
        if (!appendWord(argv[2], atoi(argv[3]))) { printf("ERROR Cannot write data/words.txt\n"); return 0; }
        printf("OK\n");
    }
    else if (strcmp(cmd, "delete") == 0 && argc >= 3) {
        toLower(argv[2]);
        TrieNode *node = findNode(argv[2]);
        if (node == NULL || !node->isEnd) { printf("ERROR Word not found\n"); return 0; }
        if (!rewriteWordsFile(argv[2], 1)) { printf("ERROR Cannot write data/words.txt\n"); return 0; }
        printf("OK\n");
    }
    else {
        printf("ERROR bad command\n");
    }
    return 0;
}
