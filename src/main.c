#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <linux/limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// data structure-------------

typedef struct KeyValueNode {
  char *key;
  char *value;
  struct KeyValueNode *next;
} KeyValueNode;

typedef struct JobNode {
  pid_t pid;
  int id;
  char *cmd;
  struct JobNode *next;
} JobNode;

typedef struct History {
  char *cmd;
  int id;
  struct History *next;
} History;

typedef struct HistoryList {
  History *head;
  int size;
} HistoryList;

typedef struct JobList {
  JobNode *head;
  JobNode *tail;
  int size;
  int jobPtr;
} JobList;

typedef struct KeyValue {
  KeyValueNode *head;
  KeyValueNode *tail;
  int size;
} KeyValue;

typedef enum {
  STD_WRITE,
  STD_APPEND,
  STD_ERR_WRITE,
  STD_ERR_APPEND,
  NIL
} RedirectType;

typedef struct Command {
  int argc;
  char *argv[64];
  char *stdoutFile;
  RedirectType redirectType;
} Command;

typedef struct Tries Tries;

typedef struct HashTable {
  Tries **buckets;
  int size;
  int capacity;
} HashTable;

typedef struct Tries {
  char value;
  bool isEnd;
  HashTable *children;
  Tries *next;
} Tries;

typedef struct MatchedResult {
  char *matched[256];
  int size;
} MatchedResult;

// - global val--------
int isError;
Tries *tries;
Tries *cTries;
KeyValue *completeKVList;
KeyValue *variableKVList;
bool isBackgroundProcess;
JobList jobList;
HistoryList historyList;
History *historyCursor;
int historyAppendCount;
// -- prototype ---------
Tries *createTriesNode();
MatchedResult *autoComplete(Tries *tries, char *prefix);
char **qSort(MatchedResult *);
char *getLcp(char **arr, int len);
void editCustomDirectoryIntoTrie(Tries *tries, char *path, int isInsert);
bool isContainSlash(char *path);
char *handlePwd();
KeyValueNode *getElement(KeyValue *kvList, char *key);
char *executeScriptAndStdOutToFile(char *scriptPath, char *cmd, char *toComp,
                                   char *beforeToComp);
void fillTrieFromLogFile(Tries *tries, char *path);
void addJob(pid_t pid, Command *cmdArr);
void cleanJobList();
// input --------
struct termios orig_termios;

void disableRawMode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

void enableRawMode() {
  tcgetattr(STDIN_FILENO, &orig_termios);

  atexit(disableRawMode);

  struct termios raw = orig_termios;

  raw.c_lflag &= ~(ECHO | ICANON);

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void readCommand(char *buffer) {
  bool isShowAllPossibility = false;
  int bufPtr = 0;
  while (1) {
    char ch;
    read(STDIN_FILENO, &ch, 1);
    // ENTER
    if (ch == '\n') {
      buffer[bufPtr] = '\0';
      write(STDOUT_FILENO, "\n", 1);
      break;
    }
    // BACKSPACE
    else if (ch == 127) {
      if (bufPtr > 0) {

        bufPtr--;
        buffer[bufPtr] = '\0';
        write(STDOUT_FILENO, "\b \b", 3);
      }
    }
    // TAB
    else if (ch == '\t') {
      bool isSlashDirAdd = false;
      char *pwd = handlePwd();
      bool addPwd = false;
      char *slashDir = NULL;
      buffer[bufPtr] = '\0';
      char *prefix = buffer;
      int index = 0;
      Tries *temTrie = NULL;
      char *COMP_LINE = strdup(buffer);
      int COMP_POINT = bufPtr;
      // Create a string buffer to hold the integer value
      char comp_point_str[32];
      snprintf(comp_point_str, sizeof(comp_point_str), "%d", COMP_POINT);

      // Now pass the string conversion to setenv
      if (setenv("COMP_LINE", COMP_LINE, 1) != 0 ||
          setenv("COMP_POINT", comp_point_str, 1) != 0) {
        perror("environment variable failed\n");
      }

      // Clean up the strdup pointer to avoid leaks later
      free(COMP_LINE);
      for (int i = 0; i < bufPtr; i++) {
        if (prefix[i] == ' ') {
          index = i + 1;
        }
      }
      prefix += index;
      bool isArgPosition = (index > 0);
      if (isContainSlash(prefix)) {
        isSlashDirAdd = true;
        slashDir = strdup(prefix);
        editCustomDirectoryIntoTrie(tries, prefix, 1);
        editCustomDirectoryIntoTrie(cTries, prefix, 1);
      }
      if (strlen(prefix) == 0 || !isContainSlash(prefix)) {
        addPwd = true;
        editCustomDirectoryIntoTrie(cTries, pwd, 1);
      }
      Tries *searchTrie = isArgPosition ? cTries : tries;

      int cmdPtr = 0;
      while (cmdPtr < bufPtr && buffer[cmdPtr] != ' ') {
        cmdPtr++;
      }
      if (cmdPtr > 0) {
        char *cmd = malloc(cmdPtr + 1);
        strncpy(cmd, buffer, cmdPtr);
        cmd[cmdPtr] = '\0';
        KeyValueNode *kvResult = getElement(completeKVList, cmd);
        if (kvResult != NULL) {
          temTrie = createTriesNode();

          // 1. Initialize fallback arguments as safe empty strings
          char *toComp = "";
          char *beforeToComp = "";

          // Create safe temporary copies of your isolated tokens
          char *tokens[64];
          int tokenCount = 0;

          char tempBuf[1024];
          snprintf(tempBuf, sizeof(tempBuf), "%.*s", bufPtr, buffer);

          // Tokenize string safely forward
          char *token = strtok(tempBuf, " ");
          while (token != NULL && tokenCount < 64) {
            tokens[tokenCount++] = token;
            token = strtok(NULL, " ");
          }

          // 2. Map indices safely depending on total tokens typed so far
          if (tokenCount >= 1) {
            // The word at the cursor is always the last token typed
            toComp = tokens[tokenCount - 1];
          }
          if (tokenCount >= 2) {
            // The preceding word context is the second-to-last token
            beforeToComp = tokens[tokenCount - 2];
          }

          // 3. Fire script safely with the clean references
          char *temFile = executeScriptAndStdOutToFile(kvResult->value, cmd,
                                                       toComp, beforeToComp);
          if (temFile != NULL) {
            fillTrieFromLogFile(temTrie, temFile);
            searchTrie = temTrie;
            unlink(temFile);
            free(temFile);
          }
        }
      }

      MatchedResult *mResult = autoComplete(searchTrie, prefix);
      if (mResult != NULL) {
        if (mResult->size == 1) {
          int remain = strlen(mResult->matched[0]) - strlen(prefix);
          if (remain > 0) {
            write(STDOUT_FILENO, mResult->matched[0] + strlen(prefix), remain);
            strcpy(buffer + index, mResult->matched[0]);
            bufPtr = strlen(buffer);
            if (buffer[bufPtr - 1] != '/') {
              write(STDOUT_FILENO, " ", 1);
              buffer[bufPtr++] = ' ';
            }
            buffer[bufPtr] = '\0';
          }
        } else {
          char **mSorted = qSort(mResult);
          char *lcp = NULL;
          int remain;
          if ((lcp = getLcp(mSorted, mResult->size)) != NULL &&
              (remain = strlen(lcp) - strlen(prefix)) > 0) {
            if (remain > 0) {
              write(STDOUT_FILENO, lcp + strlen(prefix), remain);
              strcpy(buffer + index, lcp);
              bufPtr = strlen(buffer);
            }
            free(lcp);
          } else if (isShowAllPossibility) {
            int mLen = mResult->size;
            write(STDOUT_FILENO, "\n", 1);
            for (int i = 0; i < mLen; i++) {
              write(STDOUT_FILENO, mSorted[i], strlen(mSorted[i]));
              if (i != mLen - 1) {
                write(STDOUT_FILENO, "    ", 4);
              }
            }
            write(STDOUT_FILENO, "\n", 1);
            write(STDOUT_FILENO, "$ ", 2);
            write(STDOUT_FILENO, buffer, bufPtr);
            isShowAllPossibility = false;
          } else {
            write(STDOUT_FILENO, "\x07", 1);
            isShowAllPossibility = true;
          }
        }
        // free mResult;
      } else {
        write(STDOUT_FILENO, "\x07", 1);
      }
      if (isSlashDirAdd) {
        editCustomDirectoryIntoTrie(tries, slashDir, 0);
        editCustomDirectoryIntoTrie(cTries, slashDir, 0);
      }
      if (addPwd) {
        editCustomDirectoryIntoTrie(cTries, pwd, 0);
      }
    }
    // up /down arrow
    else if (ch == 27) {
      char ch[2];
      if (read(STDIN_FILENO, &ch[0], 1) == 1 &&
          read(STDIN_FILENO, &ch[1], 1) == 1) {
        if (ch[0] == '[') {
          if (ch[1] == 'A') {
            if (historyCursor == NULL) {
              continue;
            }
            write(STDOUT_FILENO, "\33[2K\r$ ", 7);
            strcpy(buffer, historyCursor->cmd);
            bufPtr = strlen(historyCursor->cmd);
            write(STDOUT_FILENO, buffer, bufPtr);
            historyCursor = historyCursor->next;
            continue;
          } else if (ch[1] == 'B') {
            if (historyList.head == NULL || historyCursor == historyList.head) {
              continue;
            }
            History *curr = historyList.head;
            if (curr != NULL && curr->id > (historyCursor->id + 2)) {
              curr = curr->next;
            }
            write(STDOUT_FILENO, "\33[2K\r$ ", 7);
            strcpy(buffer, curr->cmd);
            bufPtr = strlen(curr->cmd);
            write(STDOUT_FILENO, buffer, bufPtr);
            historyCursor = curr->next;
            continue;
          } else {
            printf("from read command up down unkown press\n");
          }
        }
      }

    }
    // NORMAL CHAR
    else {
      buffer[bufPtr++] = ch;
      write(STDOUT_FILENO, &ch, 1);
    }
  }
}
// history handler ----
History *createHistoryNode() {
  History *newHistory = malloc(sizeof(*newHistory));
  newHistory->next = NULL;
  newHistory->cmd = NULL;
  return newHistory;
}
void addHistory(char *history) {
  History *newHistory = createHistoryNode();
  newHistory->cmd = strdup(history);
  newHistory->id = ++historyList.size;
  newHistory->next = historyList.head;
  historyList.head = newHistory;
  historyCursor = historyList.head;
}

// hashTable handler --------
HashTable *createHashTable() {
  HashTable *temHashTable = malloc(sizeof(*temHashTable));
  temHashTable->size = 0;
  temHashTable->capacity = 64;
  temHashTable->buckets = calloc(temHashTable->capacity, sizeof(Tries *));

  return temHashTable;
}

bool needResize(HashTable *hashTable) {
  int size = hashTable->size;
  int capacity = hashTable->capacity;
  float ratio = (float)size / capacity;
  if (ratio < 0.75) {
    return false;
  } else {
    return true;
  }
}

// void resizeHashTable(HashTable *hashTable) {
//   if (!needResize(hashTable)) {
//     return;
//   }
//   int currCapacity = hashTable->capacity;
//   int currSize = hashTable->size;
//   int newCapacity = currCapacity * 2;

//   Tries **newBuckets = malloc(sizeof(Tries *) * newCapacity);
//   for (int i = 0; i < currCapacity; i++) {
//     Tries curr = hashTable->buckets[i];
//     int hashIndex =
//   }
// }

int hashChar(HashTable *hashTable, char ch) {
  int chAscii = (int)ch;
  return chAscii % hashTable->capacity;
}

Tries *findRightTries(Tries *temTries, char ch) {
  if (temTries == NULL) {
    return temTries;
  }
  while (temTries != NULL) {
    if (temTries->value == ch) {
      return temTries;
    }
    temTries = temTries->next;
  }
  return temTries;
}

Tries *insertIntoHashTable(HashTable *hashTable, char ch) {
  int chHash = hashChar(hashTable, ch);
  Tries *head = hashTable->buckets[chHash];
  Tries *rightTries = findRightTries(head, ch);
  if (rightTries != NULL) {
    return rightTries;
  }

  rightTries = createTriesNode();
  rightTries->value = ch;
  rightTries->next = head;
  hashTable->buckets[chHash] = rightTries;
  hashTable->size++;

  return rightTries;
}

// tries hander ---------
Tries *createTriesNode() {
  Tries *newTries = malloc(sizeof(*newTries));
  newTries->isEnd = false;
  newTries->next = NULL;
  newTries->children = NULL;
  return newTries;
}

void insertIntoTries(Tries *tries, char *value) {
  int valueLen = strlen(value);
  Tries *curr = tries;

  for (int i = 0; i < valueLen; i++) {
    char ch = value[i];
    if (curr->children == NULL) {
      curr->children = createHashTable();
    }
    curr = insertIntoHashTable(curr->children, ch);
  }
  curr->isEnd = true;
}

bool removeFromTries(Tries *tries, char *value, int depth) {
  if (tries == NULL) {
    return false;
  }

  if (depth == strlen(value)) {
    tries->isEnd = false;
    return (tries->children == NULL);
  }

  if (tries->children == NULL) {
    return false;
  }

  char ch = value[depth];
  int hash = hashChar(tries->children, ch);

  Tries *curr = tries->children->buckets[hash];
  if (curr == NULL) {
    return false;
  }

  Tries *prev = NULL;

  while (curr != NULL && curr->value != ch) {
    prev = curr;
    curr = curr->next;
  }

  if (curr == NULL) {
    return false;
  }

  bool shouldDelete = removeFromTries(curr, value, depth + 1);

  if (shouldDelete) {
    if (prev == NULL) {
      tries->children->buckets[hash] = curr->next;
    } else {
      prev->next = curr->next;
    }
    free(curr);
  }

  bool allNull = true;

  for (int i = 0; i < tries->children->capacity; i++) {
    if (tries->children->buckets[i] != NULL) {
      allNull = false;
      break;
    }
  }

  if (allNull) {
    free(tries->children);
    tries->children = NULL;
  }
  return (!tries->isEnd && tries->children == NULL);
}

bool searchInTrie(Tries *root, char *word) {
  int wordLen = strlen(word);
  Tries *curr = root;
  for (int i = 0; i < wordLen; i++) {
    HashTable *temHashTable = curr->children;
    if (temHashTable == NULL) {
      return false;
    }
    char ch = word[i];
    Tries *head = temHashTable->buckets[hashChar(temHashTable, ch)];
    Tries *rightTries = findRightTries(head, ch);
    if (rightTries == NULL) {
      return false;
    }
    curr = rightTries;
  }
  return curr->isEnd;
}

Tries *findPrefixNode(Tries *root, char *prefix) {
  int len = strlen(prefix);
  Tries *curr = root;
  for (int i = 0; i < len; i++) {
    HashTable *temHashTable = curr->children;
    if (temHashTable == NULL) {
      return NULL;
    }
    char ch = prefix[i];
    Tries *head = temHashTable->buckets[hashChar(temHashTable, ch)];
    Tries *rightTries = findRightTries(head, ch);
    if (rightTries == NULL) {
      return NULL;
    }
    curr = rightTries;
  }
  return curr;
}
void dfscollect(Tries *node, char *buffer, int bufLen, MatchedResult *mResult) {
  if (bufLen >= 2023) {
    return;
  }
  buffer[bufLen++] = node->value;
  buffer[bufLen] = '\0';

  if (node->isEnd) {
    if (mResult->size < 256) {
      mResult->matched[mResult->size++] = strdup(buffer);
    }
  }

  HashTable *table = node->children;

  if (table == NULL) {
    return;
  }

  for (int i = 0; i < table->capacity; i++) {
    Tries *head = table->buckets[i];
    while (head) {
      dfscollect(head, buffer, bufLen, mResult);
      head = head->next;
    }
  }
}

MatchedResult *autoComplete(Tries *tries, char *prefix) {
  // if allPossiblity is 0 then return smallest possible one
  Tries *curr = findPrefixNode(tries, prefix);
  if (curr == NULL) {
    return NULL;
  }

  MatchedResult *mResult = malloc(sizeof(*mResult));
  mResult->size = 0;

  if (curr->isEnd) {
    mResult->matched[mResult->size++] = strdup(prefix);
  }

  HashTable *table = curr->children;

  if (table == NULL) {
    return mResult;
  }

  for (int i = 0; i < table->capacity; i++) {
    Tries *head = table->buckets[i];

    while (head) {
      char buffer[1024];
      strcpy(buffer, prefix);
      int len = strlen(prefix);
      dfscollect(head, buffer, len, mResult);
      head = head->next;
    }
  }
  return mResult;
}

// -- utility ---------------
char *convertToLowerCase(char *ch) {
  int chLen = strlen(ch);
  char *cmd = malloc(chLen + 1);

  int i = 0;

  for (; i < chLen; i++) {
    cmd[i] = tolower(ch[i]);
  }
  cmd[i] = '\0';
  return cmd;
}

int getPipeCount(Command *cmdArr) {
  int count = 0;
  for (int i = 0; i < cmdArr->argc; i++) {
    if (strcmp(cmdArr->argv[i], "|") == 0) {
      count++;
    }
  }
  return count;
}

bool isContainPipe(Command *cmdArr) {
  for (int i = 0; i < cmdArr->argc; i++) {
    if (strcmp(cmdArr->argv[i], "|") == 0) {
      return true;
    }
  }
  return false;
}

bool isContainSlash(char *path) {
  int pathLen = strlen(path);
  for (int i = 0; i < pathLen; i++) {
    if (path[i] == '/') {
      return true;
    }
  }
  return false;
}

int separatePathValueBySemiColon(char *pathArr[], char *path) {
  int pathLen = strlen(path);
  int pathElement = 0;
  int i = 0;
  int start;
  while (i < pathLen) {
    if (path[i] == ':') {
      i++;
      continue;
    }
    start = i;
    while (i < pathLen && path[i] != ':') {
      i++;
    }
    int len = i - start;
    char *singlePath = malloc(len + 1);
    memcpy(singlePath, path + start, len);
    singlePath[len] = '\0';
    pathArr[pathElement++] = singlePath;
  }
  return pathElement;
}

void writeOrAppendIntoFile(int type, char *buffer, char *path) {
  FILE *fp;
  if (type) {
    fp = fopen(path, "w");
  } else {
    fp = fopen(path, "a");
  }

  if (fp) {
    fprintf(fp, "%s\n", buffer);
    fclose(fp);
  }
}

void createRedirectFile(Command *cmdArr) {

  if (cmdArr->redirectType == NIL) {
    return;
  }

  FILE *fp;

  if (cmdArr->redirectType == STD_WRITE ||
      cmdArr->redirectType == STD_ERR_WRITE) {

    fp = fopen(cmdArr->stdoutFile, "w");

  } else {

    fp = fopen(cmdArr->stdoutFile, "a");
  }

  if (fp) {
    fclose(fp);
  }
}

char *isExec(char *cmd) {
  // Built-in check (case sensitive for file lookup)
  if (strcmp(cmd, "echo") == 0 || strcmp(cmd, "type") == 0 ||
      strcmp(cmd, "exit") == 0 || strcmp(cmd, "pwd") == 0) {
    return strdup(cmd);
  }

  char *pathValue = getenv("PATH");
  char *pathArr[512]; // Increased size
  int pathSize = separatePathValueBySemiColon(pathArr, pathValue);

  for (int i = 0; i < pathSize; i++) {
    char completePath[PATH_MAX];
    snprintf(completePath, sizeof(completePath), "%s/%s", pathArr[i], cmd);

    if (access(completePath, X_OK) == 0) {
      char *foundPath = strdup(completePath);
      // Clean up before returning
      for (int j = 0; j < pathSize; j++)
        free(pathArr[j]);
      return foundPath;
    }
  }

  for (int i = 0; i < pathSize; i++)
    free(pathArr[i]);
  return NULL;
}

bool lcpHelper(char **arr, int len, int pos) {
  char ch = arr[0][pos];
  for (int i = 1; i < len; i++) {
    if (arr[i][pos] != ch) {
      return false;
    }
  }
  return true;
}

char *getLcp(char **arr, int len) {
  if (len == 1) {
    return strdup(arr[0]);
  }
  int pos = 0;
  int max = strlen(arr[0]);
  for (int i = 0; i < max; i++) {
    if (lcpHelper(arr, len, pos)) {
      pos++;
    } else {
      break;
    }
  }

  if (pos != 0) {
    char *res = malloc(pos + 1);
    strncpy(res, arr[0], pos);
    res[pos] = '\0';
    return res;
  } else {
    return NULL;
  }
}

char **merge(char **left, char **right, int leftLen, int rightLen) {

  char **res = malloc(sizeof(char *) * (leftLen + rightLen));

  int i = 0;
  int j = 0;
  int k = 0;

  while (i < leftLen && j < rightLen) {

    if (strcmp(left[i], right[j]) <= 0) {

      res[k++] = strdup(left[i++]);

    } else {

      res[k++] = strdup(right[j++]);
    }
  }

  while (i < leftLen) {
    res[k++] = strdup(left[i++]);
  }

  while (j < rightLen) {
    res[k++] = strdup(right[j++]);
  }

  return res;
}

char **sMergeSort(char **arr, int len) {

  if (len == 1) {

    char **base = malloc(sizeof(char *));
    base[0] = strdup(arr[0]);

    return base;
  }

  int mid = len / 2;

  char **left = sMergeSort(arr, mid);

  char **right = sMergeSort(arr + mid, len - mid);

  return merge(left, right, mid, len - mid);
}

char **qSort(MatchedResult *mResult) {

  char **mArray = malloc(sizeof(char *) * mResult->size);

  for (int i = 0; i < mResult->size; i++) {

    mArray[i] = strdup(mResult->matched[i]);
  }

  return sMergeSort(mArray, mResult->size);
}

// kvList -------
KeyValue *createKVList() {
  KeyValue *newKeyValue = malloc(sizeof(*newKeyValue));
  newKeyValue->head = NULL;
  newKeyValue->tail = NULL;
  newKeyValue->size = 0;
  return newKeyValue;
}
KeyValueNode *getElement(KeyValue *kvList, char *key) {
  if (kvList->size < 1 || kvList->head == NULL) {
    return NULL;
  }
  KeyValueNode *curr = kvList->head;
  while (curr != NULL) {
    if (strcmp(curr->key, key) == 0) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
}

KeyValueNode *insertElement(KeyValue *kvList, char *key, char *value) {
  if (key == NULL || value == NULL) {
    return NULL;
  }
  KeyValueNode *newKVNode = malloc(sizeof(*newKVNode));
  newKVNode->key = strdup(key);
  newKVNode->value = strdup(value);
  newKVNode->next = NULL;

  if (kvList->head == NULL) {
    kvList->head = newKVNode;
    kvList->tail = newKVNode;
  } else {
    kvList->tail->next = newKVNode;
    kvList->tail = newKVNode;
  }
  kvList->size++;
  return newKVNode;
}

bool removeElement(KeyValue *kvList, char *key) {
  if (kvList->size < 1 || kvList->head == NULL) {
    return false;
  }
  KeyValueNode *curr = kvList->head;
  KeyValueNode *pre = NULL;
  while (curr != NULL) {
    if (strcmp(curr->key, key) == 0) {
      KeyValueNode *toFree = curr;
      if (pre != NULL) {
        pre->next = curr->next;
      } else {
        kvList->head = curr->next;
      }
      if (curr == kvList->tail) {
        kvList->tail = pre;
      }
      kvList->size--;
      return true;
    }
    pre = curr;
    curr = curr->next;
  }
  return false;
}

// parsers ------------
Command *parser(char *rawCmdStr) {
  Command *cmdArr = malloc(sizeof(*cmdArr));
  cmdArr->argc = 0;
  cmdArr->stdoutFile = NULL;
  cmdArr->redirectType = NIL;

  int len = strlen(rawCmdStr);
  int i = 0;

  while (i < len) {
    while (i < len && rawCmdStr[i] == ' ') {
      i++;
    }
    if (i >= len) {
      break;
    }
    char buffer[1024];
    int bufPtr = 0;
    while (i < len && rawCmdStr[i] != ' ') {
      if (rawCmdStr[i] == '\\') {
        i++;
        buffer[bufPtr++] = rawCmdStr[i++];
      } else if (rawCmdStr[i] == '\'') {
        i++;
        while (i < len && rawCmdStr[i] != '\'') {
          buffer[bufPtr++] = rawCmdStr[i++];
        }
        if (i < len && rawCmdStr[i] == '\'') {
          i++;
        }
      } else if (rawCmdStr[i] == '\"') {
        i++;
        while (i < len && rawCmdStr[i] != '\"') {
          if (rawCmdStr[i] == '\\') {
            i++;
            buffer[bufPtr++] = rawCmdStr[i++];
            continue;
          }
          buffer[bufPtr++] = rawCmdStr[i++];
        }
        if (i < len && rawCmdStr[i] == '\"') {
          i++;
        }
      } else if (rawCmdStr[i] == '$') {
        i++;
        bool isCurly = false;
        if (rawCmdStr[i] == '{') {
          isCurly = true;
          i++;
        }
        char var[256];
        int varPtr = 0;
        while (i < len && rawCmdStr[i] != ' ' && rawCmdStr[i] != '}') {
          var[varPtr++] = rawCmdStr[i++];
        }
        if (isCurly) {
          i++;
        }
        var[varPtr] = '\0';
        KeyValueNode *result = getElement(variableKVList, var);
        if (result != NULL) {
          bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1, "%s",
                             result->value);
        } else {
        }
      } else {
        buffer[bufPtr++] = rawCmdStr[i++];
      }
    }
    if (bufPtr == 0) {
      continue;
    }
    buffer[bufPtr] = '\0';
    if (strcmp(buffer, ">") == 0 || strcmp(buffer, "1>") == 0 ||
        strcmp(buffer, "2>") == 0 || strcmp(buffer, ">>") == 0 ||
        strcmp(buffer, "1>>") == 0 || strcmp(buffer, "2>>") == 0) {
      if (strcmp(buffer, ">") == 0 || strcmp(buffer, "1>") == 0) {
        cmdArr->redirectType = STD_WRITE;
      } else if (strcmp(buffer, "2>") == 0) {
        cmdArr->redirectType = STD_ERR_WRITE;
      } else if (strcmp(buffer, ">>") == 0 || strcmp(buffer, "1>>") == 0) {
        cmdArr->redirectType = STD_APPEND;
      } else {
        cmdArr->redirectType = STD_ERR_APPEND;
      }
      while (i < len && rawCmdStr[i] == ' ') {
        i++;
      }
      char fileBuffer[1024];
      int fileBufPtr = 0;
      while (i < len && rawCmdStr[i] != ' ') {
        fileBuffer[fileBufPtr++] = rawCmdStr[i++];
      }
      fileBuffer[fileBufPtr] = '\0';
      cmdArr->stdoutFile = malloc(fileBufPtr + 1);
      memcpy(cmdArr->stdoutFile, fileBuffer, fileBufPtr);
      cmdArr->stdoutFile[fileBufPtr] = '\0';
    } else {
      char *arg = malloc(bufPtr + 1);
      memcpy(arg, buffer, bufPtr);
      arg[bufPtr] = '\0';
      cmdArr->argv[cmdArr->argc++] = arg;
    }
  }

  return cmdArr;
}

bool isKeyValid(char *key) {
  int len = strlen(key);
  if (len == 0) {
    return false;
  }
  if (!((key[0] >= (int)'A' && key[0] <= (int)'Z') ||
        (key[0] >= (int)'a' && key[0] <= (int)'z') || key[0] == '_')) {
    return false;
  }
  int i = 1;
  while (i < len && key[i] != '=') {
    if (!((key[i] >= (int)'A' && key[i] <= (int)'Z') ||
          (key[i] >= (int)'a' && key[i] <= (int)'z') ||
          (key[i] >= (int)'0' && key[i] <= (int)'9') || key[i] == '_')) {
      return false;
    }
    i++;
  }
  return true;
}

// cmd Handler --------------

char *handlePwd() {
  char *pwd = malloc(PATH_MAX);
  getcwd(pwd, PATH_MAX);
  return pwd;
}

char *handleBackgroundPrcess(Command *cmdArr) {
  // 1. Flush stdout/stderr BEFORE forking so buffered bytes aren't duplicated
  fflush(stdout);
  fflush(stderr);

  // Resolve the full binary path (e.g., "cat" -> "/usr/bin/cat")
  char *fullPath = isExec(cmdArr->argv[0]);

  pid_t pid = fork();

  if (pid < 0) {
    perror("fork failed");
    if (fullPath)
      free(fullPath);
    return strdup("");
  }
  addJob(pid, cmdArr);
  if (pid == 0) {
    // --- CHILD PROCESS ---
    // Make sure the last argument position in argv array is strictly NULL
    // terminated
    cmdArr->argv[cmdArr->argc] = NULL;

    // DO NOT close or redirect STDOUT/STDERR here!
    // By leaving them alone, they inherit the shell's terminal attachment
    // automatically.

    if (fullPath) {
      execv(fullPath, cmdArr->argv);
    } else {
      // Fallback search mechanism using PATH environment
      execvp(cmdArr->argv[0], cmdArr->argv);
    }

    // CRITICAL GUARD: If execution fails, terminate the child immediately
    // so it doesn't leak into the shell loop and corrupt standard output
    // streams.
    perror("exec failed");
    exit(1);
  }

  // --- PARENT PROCESS ---
  if (fullPath) {
    free(fullPath);
  }

  // CodeCrafters format expects: "[job_id] process_id"
  char buffer[1024];
  int bufPtr = snprintf(buffer, sizeof(buffer), "[%d] %d", jobList.size, pid);
  buffer[bufPtr] = '\0';

  return strdup(buffer);
}
char *handleEcho(Command *cmdArr) {
  int i = 1;
  char *buffer = malloc(1025);
  int bufPtr = 0;
  while (i < cmdArr->argc) {
    bufPtr += snprintf(buffer + bufPtr, 1024 - bufPtr, "%s", cmdArr->argv[i]);
    if (i != cmdArr->argc - 1) {
      bufPtr += snprintf(buffer + bufPtr, 1024 - bufPtr, " ");
    }
    i++;
  }
  buffer[bufPtr] = '\0';
  return buffer;
}

bool isBuiltIn(char *cmd) {
  if (strcmp(cmd, "type") == 0 || strcmp(cmd, "exit") == 0 ||
      strcmp(cmd, "pwd") == 0 || strcmp(cmd, "complete") == 0 ||
      strcmp(cmd, "jobs") == 0) {
    return true;
  } else {
    return false;
  }
}
char *handleType(char *cmd) {
  char *buffer = malloc(1025);
  int bufPtr = 0;
  if (strcmp(cmd, "echo") == 0 || strcmp(cmd, "type") == 0 ||
      strcmp(cmd, "exit") == 0 || strcmp(cmd, "pwd") == 0 ||
      strcmp(cmd, "complete") == 0 || strcmp(cmd, "jobs") == 0 ||
      strcmp(cmd, "history") == 0 || strcmp(cmd, "declare") == 0) {
    bufPtr +=
        snprintf(buffer + bufPtr, 1024 - bufPtr, "%s is a shell builtin", cmd);
  } else {
    char *pathValue = getenv("PATH");
    char *pathArr[64];
    int pathSize = separatePathValueBySemiColon(pathArr, pathValue);

    int found = 0;
    for (int i = 0; i < pathSize; i++) {
      char completePath[256];
      int completePathSize = snprintf(completePath, sizeof(completePath),
                                      "%s/%s", pathArr[i], cmd);
      completePath[completePathSize] = '\0';
      if (access(completePath, X_OK) == 0) {
        bufPtr += snprintf(buffer + bufPtr, 1024 - bufPtr, "%s is %s", cmd,
                           completePath);
        found = 1;
        break;
      }
    }
    if (!found) {
      isError = 1;
      bufPtr += snprintf(buffer + bufPtr, 1024 - bufPtr, "%s: not found", cmd);
    }
    for (int i = 0; i < pathSize; i++) {
      free(pathArr[i]);
    }
  }
  buffer[bufPtr] = '\0';
  return buffer;
}

char *handleCd(char *path) {
  if (strcmp(path, "~") == 0) {
    path = getenv("HOME");
  }
  char *prevPath = handlePwd();
  char *buff = NULL;
  if (chdir(path) != 0) {
    isError = 1;
    char notFound[1024];
    int notFoundPtr = 0;
    notFoundPtr = snprintf(notFound, sizeof(notFound),
                           "%s: No such file or directory", path);
    notFound[notFoundPtr] = '\0';
    buff = malloc(notFoundPtr + 1);
    memcpy(buff, notFound, notFoundPtr);
    buff[notFoundPtr] = '\0';
  }
  if (!isError) {
    editCustomDirectoryIntoTrie(tries, prevPath, 0);
    editCustomDirectoryIntoTrie(tries, ".", 1);
    editCustomDirectoryIntoTrie(cTries, prevPath, 0);
    editCustomDirectoryIntoTrie(cTries, ".", 1);
  }
  return buff;
}

char *executeScriptAndStdOutToFile(char *scriptPath, char *cmd, char *toComp,
                                   char *beforeToComp) {
  char template[] = "/tmp/shell_completion_XXXXXX";

  int temFd = mkstemp(template);
  if (temFd == -1) {
    perror("mkstemp failed: from executeScriptAndStdOutToFile\n");
    return NULL;
  }

  pid_t pid = fork();

  if (pid < 0) {
    perror("fork failed\n");
    return NULL;
  }

  if (pid == 0) {
    if (dup2(temFd, STDOUT_FILENO) == -1) {
      perror("dup2 failed");
      _exit(1);
    }
    close(temFd);
    char *scriptArgs[] = {scriptPath, cmd, toComp, beforeToComp, NULL};
    execv(scriptPath, scriptArgs);
    perror("execv failed");
    _exit(1);
  } else {
    close(temFd);
    int status;
    waitpid(pid, &status, 0);
    return strdup(template);
  }
}

void fillTrieFromLogFile(Tries *tries, char *path) {
  FILE *fd = fopen(path, "r");

  if (fd == NULL) {
    perror("fopen failed\n");
    return;
  }
  char buffer[1024];

  while (fgets(buffer, sizeof(buffer), fd) != NULL) {
    buffer[strcspn(buffer, "\n")] = '\0';
    insertIntoTries(tries, buffer);
  }
  fclose(fd);
  return;
}

char *handleComplete(Command *cmdArr) {
  int ptr = 1;
  char buffer[1025];
  int bufPtr = 0;
  if (strcmp(cmdArr->argv[ptr], "-p") == 0) {
    ptr++;
    KeyValueNode *getResult = getElement(completeKVList, cmdArr->argv[ptr]);
    if (getResult != NULL) {
      bufPtr += snprintf(buffer, 1024 - bufPtr, "complete -C '%s' %s",
                         getResult->value, cmdArr->argv[ptr]);
    } else {
      bufPtr += snprintf(buffer, 1024 - bufPtr,
                         "complete: %s: no completion specification",
                         cmdArr->argv[ptr]);
    }
    buffer[bufPtr] = '\0';
    return strdup(buffer);
  } else if (strcmp(cmdArr->argv[ptr], "-C") == 0) {
    ptr++;
    KeyValueNode *insertResult =
        insertElement(completeKVList, cmdArr->argv[ptr + 1], cmdArr->argv[ptr]);
    return NULL;
  } else if (strcmp(cmdArr->argv[ptr], "-r") == 0) {
    ptr++;
    if (ptr >= cmdArr->argc) {
      return NULL;
    }
    bool rmResult = removeElement(completeKVList, cmdArr->argv[ptr]);
    return NULL;
  }
  return NULL;
}

void addJob(pid_t pid, Command *cmdArr) {
  JobNode *newJob = malloc(sizeof(*newJob));
  newJob->pid = pid;
  newJob->next = NULL;
  char buffer[1024];
  int bufPtr = 0;
  for (int i = 0; i < cmdArr->argc; i++) {
    bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1, "%s",
                       cmdArr->argv[i]);
    if (i != cmdArr->argc) {
      bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1, " ");
    }
  }
  buffer[bufPtr] = '\0';
  newJob->cmd = strdup(buffer);
  JobNode *curr = jobList.head;
  JobNode *prev = NULL;
  int i = 1;
  while (curr != NULL) {
    if (curr->id != i) {
      break;
    }
    i++;
    prev = curr;
    curr = curr->next;
  }
  newJob->id = i;
  if (prev == NULL) {
    jobList.head = newJob;
    jobList.tail = newJob;
  } else {
    JobNode *next = prev->next;
    prev->next = newJob;
    newJob->next = next;
    if (jobList.tail == prev) {
      jobList.tail = newJob;
    }
  }
  jobList.size++;
}

void cleanJobList() {
  JobNode *curr = jobList.head;
  if (curr == NULL) {
    return;
  }
  JobNode *prev = NULL;
  while (curr != NULL) {
    int status;
    int isCont = 0;
    int result = waitpid(curr->pid, &status, WNOHANG);
    if (result > 0) {
      JobNode *toFree = curr;
      if (prev == NULL) {
        isCont = 1;
        curr = curr->next;
        jobList.head = curr;
      } else {
        prev->next = curr->next;
      }
      if (jobList.tail == curr) {
        jobList.tail = prev;
      }
      free(toFree->cmd);
      free(toFree);
      jobList.size--;
    }
    if (isCont) {
      continue;
    }
    prev = curr;
    curr = curr->next;
  }
}

void deleteFromJobList(pid_t pid) {
  JobNode *curr = jobList.head;
  if (curr == NULL) {
    return;
  }
  JobNode *prev = NULL;
  while (curr != NULL) {
    if (curr->pid == pid) {
      JobNode *toFree = curr;
      if (prev == NULL) {
        curr = curr->next;
        jobList.head = curr;
      } else {
        prev->next = curr->next;
      }
      if (jobList.tail == toFree) {
        jobList.tail = prev;
      }
      free(toFree->cmd);
      free(toFree);
      jobList.size--;
      return;
    }
    prev = curr;
    curr = curr->next;
  }
}

char *handleJobs() {
  char buffer[1024];
  int bufPtr = 0;
  JobNode *head = jobList.head;
  if (head == NULL) {
    return NULL;
  }
  int toFree[128];
  int toFreePtr = 0;
  int len = jobList.size;
  int count = 1;
  while (head != NULL) {
    int status;
    int result = waitpid(head->pid, &status, WNOHANG);
    char *what =
        result > 0 ? "Done                    " : "Running                 ";
    char tem = count == len ? '+' : count == (len - 1) ? '-' : ' ';
    bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1,
                       "[%d]%c   %s%s", head->id, tem, what, head->cmd);
    if (count != len) {
      bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1, "\n");
    }
    if (strcmp(what, "Done                    ") == 0) {
      toFree[toFreePtr++] = head->pid;
    }
    count++;
    head = head->next;
  }
  for (int i = 0; i < toFreePtr; i++) {
    deleteFromJobList(toFree[i]);
  }
  buffer[bufPtr] = '\0';
  return strdup(buffer);
}

void fillNumberInCharArray(char *id, int len, int num) {
  len--;
  id[len--] = '\0';
  while (num > 0) {
    int rem = num % 10;
    id[len--] = rem + '0';
    num /= 10;
  }
  while (len >= 0) {
    id[len--] = ' ';
  }
}

History *reverseList(History **start, History *head) {
  if (head == NULL) {
    return NULL;
  }
  History *newHead = createHistoryNode();
  newHead->cmd = strdup(head->cmd);
  newHead->id = head->id;
  newHead->next = NULL;
  if (head->next == NULL) {
    *start = newHead;
    return newHead;
  }
  History *prev = reverseList(start, head->next);
  prev->next = newHead;
  return newHead;
}

char *handleHistory(Command *cmdArr) {
  if (cmdArr->argc == 3 && strcmp(cmdArr->argv[1], "-r") == 0) {
    char *historyFile = cmdArr->argv[2];
    FILE *fd = fopen(historyFile, "r");
    if (fd == NULL) {
      perror("fopen failed\n");
      return NULL;
    }
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), fd) != NULL) {
      buffer[strcspn(buffer, "\n")] = '\0';
      addHistory(buffer);
    }
    fclose(fd);
    return NULL;
  }
  if (cmdArr->argc == 3 && strcmp(cmdArr->argv[1], "-w") == 0) {
    char *historyFile = cmdArr->argv[2];
    FILE *fd = fopen(historyFile, "w");
    if (fd == NULL) {
      perror("fopen failed\n");
      return NULL;
    }
    History *curr = NULL;
    History *tail = reverseList(&curr, historyList.head);
    while (curr != NULL) {
      fprintf(fd, "%s\n", curr->cmd);
      curr = curr->next;
    }
    historyAppendCount = historyList.size;
    fclose(fd);
    return NULL;
  }
  if (cmdArr->argc == 3 && strcmp(cmdArr->argv[1], "-a") == 0) {
    char *historyFile = cmdArr->argv[2];
    FILE *fd = fopen(historyFile, "a");
    if (fd == NULL) {
      perror("fopen failed\n");
      return NULL;
    }
    History *curr = NULL;
    History *tail = reverseList(&curr, historyList.head);
    while (curr != NULL) {
      if (curr->id > historyAppendCount) {
        break;
      }
      curr = curr->next;
    }
    while (curr != NULL) {
      fprintf(fd, "%s\n", curr->cmd);
      curr = curr->next;
    }
    historyAppendCount = historyList.size;
    fclose(fd);
    return NULL;
  }
  char buffer[1024];
  int bufPtr = 0;
  History *curr = NULL;
  History *tail = reverseList(&curr, historyList.head);
  int size = historyList.size;
  int n = -1;
  if (cmdArr->argc == 2) {
    n = atoi(cmdArr->argv[1]);
    n = size - n;
  }
  int i = 0;
  while (curr != NULL && i < size) {
    if (i < n) {
      i++;
      curr = curr->next;
      continue;
    }
    char id[5];
    fillNumberInCharArray(id, sizeof(id), curr->id);
    bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1, "%s  %s",
                       id, curr->cmd);
    if (curr->next != NULL) {
      bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1, "\n");
    }
    i++;
    curr = curr->next;
  }
  buffer[bufPtr] = '\0';
  return strdup(buffer);
}

char *getDoneJob() {
  char buffer[1024];
  int bufPtr = 0;
  bool isThereAny = false;
  JobNode *head = jobList.head;
  if (head == NULL) {
    return NULL;
  }
  int toFree[128];
  int toFreePtr = 0;
  int len = jobList.size;
  int count = 1;
  while (head != NULL) {
    int status;
    int result = waitpid(head->pid, &status, WNOHANG);
    if (result == 0) {
      count++;
      head = head->next;
      continue;
    }
    isThereAny = true;
    char *what =
        result > 0 ? "Done                    " : "Running                 ";
    char tem = count == len ? '+' : count == (len - 1) ? '-' : ' ';
    bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1,
                       "[%d]%c   %s%s", head->id, tem, what, head->cmd);
    if (count != len) {
      bufPtr += snprintf(buffer + bufPtr, sizeof(buffer) - bufPtr - 1, "\n");
    }
    if (strcmp(what, "Done                    ") == 0) {
      toFree[toFreePtr++] = head->pid;
    }
    count++;
    head = head->next;
  }
  for (int i = 0; i < toFreePtr; i++) {
    deleteFromJobList(toFree[i]);
  }
  buffer[bufPtr] = '\0';
  if (isThereAny) {
    return strdup(buffer);
  } else {
    return NULL;
  }
}

void populateTriesWithBuildInCommand(Tries *tries) {
  insertIntoTries(tries, "echo");
  insertIntoTries(tries, "type");
  insertIntoTries(tries, "exit");
  insertIntoTries(tries, "pwd");
}

void populateExecutablesIntoTrie(Tries *tries) {

  char *pathValue = getenv("PATH");

  if (pathValue == NULL) {
    return;
  }

  char *pathArr[128];

  int pathSize = separatePathValueBySemiColon(pathArr, pathValue);

  for (int i = 0; i < pathSize; i++) {

    DIR *dir = opendir(pathArr[i]);

    if (dir == NULL) {
      continue;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {

      // skip hidden files
      if (entry->d_name[0] == '.') {
        continue;
      }

      char fullPath[PATH_MAX];

      snprintf(fullPath, sizeof(fullPath), "%s/%s", pathArr[i], entry->d_name);

      if (access(fullPath, X_OK) == 0) {

        insertIntoTries(tries, entry->d_name);
      }
    }

    closedir(dir);
  }

  for (int i = 0; i < pathSize; i++) {
    free(pathArr[i]);
  }
}

// Returns the directory portion of pathValue as a filesystem path.
// isRoot=1: pathValue is absolute, strip last component.
// isRoot=0: pathValue is relative, prepend cwd then strip last component.
char *getTrimedPathValue(char *pathValue, int isRoot) {
  char path[1024];
  int k = 0;
  // Scan backward to find the last '/' (was i++ — infinite loop)
  for (int i = (int)strlen(pathValue) - 1; i >= 0; i--) {
    if (pathValue[i] == '/') {
      k = i;
      break;
    }
  }
  int pathPtr = 0;
  if (isRoot) {
    strncpy(path, pathValue, k);
    pathPtr = k;
  } else {
    char *pwd = handlePwd();
    int pwdLen = strlen(pwd);
    strcpy(path, pwd);
    pathPtr = pwdLen;
    path[pathPtr++] = '/';
    strncpy(path + pathPtr, pathValue, k);
    pathPtr += k;
    free(pwd);
  }
  path[pathPtr] = '\0';
  return strdup(path);
}

void editCustomDirectoryIntoTrie(Tries *tries, char *pathValue, int isInsert) {
  char dirPath[PATH_MAX];    // filesystem path to opendir()
  char triePrefix[PATH_MAX]; // prepended to entry name when inserting/removing

  struct stat st;
  int pvLen = strlen(pathValue);

  // Case 1: pathValue ends with '/' — e.g. "banana/strawberry/" typed by user.
  //         The whole string is both the directory to open and the trie prefix,
  //         so entries are stored as "banana/strawberry/mango.txt".
  if (pvLen > 0 && pathValue[pvLen - 1] == '/') {
    strncpy(dirPath, pathValue, PATH_MAX - 1);
    dirPath[PATH_MAX - 1] = '\0';
    strncpy(triePrefix, pathValue, PATH_MAX - 1);
    triePrefix[PATH_MAX - 1] = '\0';

    // Case 2: pathValue IS a plain directory ("." from main/handleCd, or an
    //         absolute prevPath like "/home/user/olddir").
    //         Open it directly; trie keys are bare entry names.
  } else if (strcmp(pathValue, ".") == 0 ||
             (stat(pathValue, &st) == 0 && S_ISDIR(st.st_mode))) {
    strncpy(dirPath, pathValue, PATH_MAX - 1);
    dirPath[PATH_MAX - 1] = '\0';
    triePrefix[0] = '\0';

    // Case 3: pathValue is a partial nested path from TAB completion,
    //         e.g. "path/to/f". Split at last '/' to get the directory
    //         ("path/to") and the prefix to store in trie ("path/to/").
  } else if (isContainSlash(pathValue)) {
    char *lastSlash = strrchr(pathValue, '/');
    int prefixLen = (int)(lastSlash - pathValue) + 1; // includes the '/'

    // triePrefix = "path/to/"  (everything up to and including last slash)
    strncpy(triePrefix, pathValue, prefixLen);
    triePrefix[prefixLen] = '\0';

    // dirPath = "path/to"  (opendir works with relative paths)
    strncpy(dirPath, pathValue, prefixLen - 1);
    dirPath[prefixLen - 1] = '\0';

  } else {
    // No slash and not a directory — nothing meaningful to open.
    return;
  }

  DIR *dir = opendir(dirPath);
  if (dir == NULL) {
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;

    // Build the filesystem path to stat the entry
    char entryPath[PATH_MAX];
    snprintf(entryPath, sizeof(entryPath), "%s/%s", dirPath, entry->d_name);

    // Append '/' if it's a directory, so completion lands at "banana/" not
    // "banana "
    struct stat entrySt;
    char trieKey[PATH_MAX];
    if (stat(entryPath, &entrySt) == 0 && S_ISDIR(entrySt.st_mode)) {
      snprintf(trieKey, sizeof(trieKey), "%s%s/", triePrefix, entry->d_name);
    } else {
      snprintf(trieKey, sizeof(trieKey), "%s%s", triePrefix, entry->d_name);
    }

    if (isInsert) {
      insertIntoTries(tries, trieKey);
    } else {
      removeFromTries(tries, trieKey, 0);
    }
  }
  closedir(dir);
}

char *handleDeclare(Command *cmdArr) {
  char buffer[1024];
  int bufPtr = 0;
  if (cmdArr->argc > 2 && strcmp(cmdArr->argv[1], "-p") == 0) {
    char *key = cmdArr->argv[2];
    KeyValueNode *result = getElement(variableKVList, key);
    if (result == NULL) {
      bufPtr += snprintf(buffer, sizeof(buffer) - bufPtr,
                         "declare: %s: not found", key);
      buffer[bufPtr] = '\0';
    } else {
      bufPtr += snprintf(buffer, sizeof(buffer) - bufPtr,
                         "declare -- %s=\"%s\"", key, result->value);
      buffer[bufPtr] = '\0';
    }
    return strdup(buffer);
  }
  char *KVpair = cmdArr->argv[1];
  int len = strlen(KVpair);
  char *key;
  char *value;
  int i = 0;
  while (i < len) {
    if (KVpair[i] != '=') {
      i++;
      continue;
    }
    key = malloc(i + 1);
    strncpy(key, KVpair, i);
    key[i] = '\0';
    if (i + 1 < len) {
      i++;
    }
    value = malloc(len - i + 1);
    strncpy(value, KVpair + i, len - i);
    value[len - i] = '\0';
  }
  if (!isKeyValid(key)) {
    bufPtr += snprintf(buffer, sizeof(buffer) - bufPtr,
                       "declare: `%s': not a valid identifier", KVpair);
    buffer[bufPtr] = '\0';
    return strdup(buffer);
  }
  KeyValueNode *res = getElement(variableKVList, key);
  if (res != NULL) {
    free(res->value);
    res->value = value;
    return NULL;
  }
  KeyValueNode *result = insertElement(variableKVList, key, value);
  return NULL;
}

// cmdHandler----------
char *cmdHandler(Command *cmdArr) {
  isBackgroundProcess = false;
  isError = 0;
  createRedirectFile(cmdArr);
  if (strcmp(cmdArr->argv[cmdArr->argc - 1], "&") == 0) {
    isBackgroundProcess = true;
    free(cmdArr->argv[cmdArr->argc - 1]);
    cmdArr->argc--;
  }

  char *cmd = convertToLowerCase(cmdArr->argv[0]);

  char *buffer = NULL;
  if (isBackgroundProcess) {
    buffer = handleBackgroundPrcess(cmdArr);
  } else if (strcmp(cmd, "echo") == 0) {
    buffer = handleEcho(cmdArr);
  } else if (strcmp(cmd, "type") == 0) {
    buffer = handleType(cmdArr->argv[1]);
  } else if (strcmp(cmd, "pwd") == 0) {
    buffer = handlePwd();
  } else if (strcmp(cmd, "cd") == 0) {
    buffer = handleCd(cmdArr->argv[1]);
  } else if (strcmp(cmd, "complete") == 0) {
    buffer = handleComplete(cmdArr);
  } else if (strcmp(cmd, "jobs") == 0) {
    buffer = handleJobs();
  } else if (strcmp(cmd, "history") == 0) {
    buffer = handleHistory(cmdArr);
  } else if (strcmp(cmd, "declare") == 0) {
    buffer = handleDeclare(cmdArr);
  } else {
    char *fullPath = isExec(cmdArr->argv[0]);
    if (fullPath) {
      pid_t pid = fork();
      if (pid == 0) {
        if (cmdArr->redirectType == STD_WRITE ||
            cmdArr->redirectType == STD_APPEND ||
            cmdArr->redirectType == STD_ERR_WRITE ||
            cmdArr->redirectType == STD_ERR_APPEND) {
          FILE *fp;
          if (cmdArr->redirectType == STD_WRITE ||
              cmdArr->redirectType == STD_ERR_WRITE) {
            fp = fopen(cmdArr->stdoutFile, "w");
          } else {
            fp = fopen(cmdArr->stdoutFile, "a");
          }
          if (!fp) {
            perror("fopen");
            _exit(1);
          }
          if (cmdArr->redirectType == STD_WRITE ||
              cmdArr->redirectType == STD_APPEND) {
            dup2(fileno(fp), STDOUT_FILENO);
          } else {
            dup2(fileno(fp), STDERR_FILENO);
          }
          fclose(fp);
        }
        cmdArr->argv[cmdArr->argc] = NULL; // NULL terminate
        execv(fullPath, cmdArr->argv);
        _exit(1);
      } else {
        int status;
        // Wait ONLY for this specific foreground process
        waitpid(pid, &status, 0);
        fflush(stdout);
      }
      free(fullPath);
    } else {
      isError = 1;
      char notFound[1024];
      int notFoundPtr = 0;
      notFoundPtr = snprintf(notFound, sizeof(notFound),
                             "%s: command not found", cmdArr->argv[0]);
      notFound[notFoundPtr] = '\0';
      buffer = malloc(notFoundPtr + 1);
      strcpy(buffer, notFound);
    }
  }
  if (buffer != NULL) {
    if (cmdArr->redirectType == STD_WRITE ||
        (cmdArr->redirectType == STD_ERR_WRITE && isError)) {
      writeOrAppendIntoFile(1, buffer, cmdArr->stdoutFile);
      return NULL;
    } else if (cmdArr->redirectType == STD_APPEND ||
               (cmdArr->redirectType == STD_ERR_APPEND && isError)) {
      writeOrAppendIntoFile(0, buffer, cmdArr->stdoutFile);
      return NULL;
    } else {
      return buffer;
    }
  }
  return buffer;
}

// pipe handler-------
void handlePipe(Command *CommandArray[], int CommandArrayLen) {

  int prevRead = -1;

  pid_t pids[128];

  for (int i = 0; i < CommandArrayLen; i++) {
    int fd[2];

    bool hasNext = (i != CommandArrayLen - 1);

    if (hasNext) {
      pipe(fd);
    }

    pid_t pid = fork();
    if (pid == 0) {
      // Read from previous pipe stage channel
      if (prevRead != -1) {
        dup2(prevRead, STDIN_FILENO);
        close(prevRead);
      }

      // Write into the next pipeline stage channel
      if (hasNext) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
      }

      Command *cmd = CommandArray[i];

      // Handle custom file redirections if present
      if (cmd->redirectType == STD_WRITE || cmd->redirectType == STD_APPEND) {
        char *mode = (cmd->redirectType == STD_WRITE) ? "w" : "a";
        FILE *fp = fopen(cmd->stdoutFile, mode);
        if (fp) {
          dup2(fileno(fp), STDOUT_FILENO);
          fclose(fp);
        }
      }

      // 1. BUILT-IN COMMAND HANDLING INSIDE PIPELINE
      char *lowerCmd = convertToLowerCase(cmd->argv[0]);

      if (strcmp(lowerCmd, "echo") == 0) {
        char *res = handleEcho(cmd);
        if (res) {
          printf("%s\n", res);
          free(res);
        }
        free(lowerCmd);
        _exit(0);
      } else if (strcmp(lowerCmd, "pwd") == 0) {
        char *res = handlePwd();
        if (res) {
          printf("%s\n", res);
          free(res);
        }
        free(lowerCmd);
        _exit(0);
      } else if (strcmp(lowerCmd, "type") == 0) {
        char *res = handleType(cmd->argv[1]);
        if (res) {
          printf("%s\n", res);
          free(res);
        }
        free(lowerCmd);
        _exit(0);
      }
      free(lowerCmd);

      // 2. EXTERNAL BINARIES FALLBACK
      cmd->argv[cmd->argc] = NULL;
      char *path = isExec(cmd->argv[0]);
      if (!path) {
        fprintf(stderr, "%s: command not found\n", cmd->argv[0]);
        _exit(1);
      }
      execv(path, cmd->argv);
      perror("execv failed");
      _exit(1);
    }

    pids[i] = pid;

    // parent

    if (prevRead != -1) {
      close(prevRead);
    }

    if (hasNext) {

      close(fd[1]);

      prevRead = fd[0];
    }
  }

  for (int i = 0; i < CommandArrayLen; i++) {
    waitpid(pids[i], NULL, 0);
  }
}

void loadHistory() {
  char *historyFile = getenv("HISTFILE");
  if (historyFile == NULL) {
    return;
  }
  FILE *fd = fopen(historyFile, "r");
  if (fd == NULL) {
    perror("fopen failed\n");
    return;
  }
  char buffer[1024];
  while (fgets(buffer, sizeof(buffer), fd) != NULL) {
    buffer[strcspn(buffer, "\n")] = '\0';
    addHistory(buffer);
  }
  fclose(fd);
  return;
}

void writeHistory() {
  char *historyFile = getenv("HISTFILE");
  if (historyFile == NULL) {
    return;
  }
  FILE *fd = fopen(historyFile, "w");
  if (fd == NULL) {
    perror("fopen failed\n");
    return;
  }
  History *curr = NULL;
  History *tail = reverseList(&curr, historyList.head);
  while (curr != NULL) {
    fprintf(fd, "%s\n", curr->cmd);
    curr = curr->next;
  }
  fclose(fd);
  return;
}

// -- main ---------------------------------
int main(int argc, char *argv[]) {
  // populate tries with build in command
  completeKVList = createKVList();
  variableKVList = createKVList();
  tries = createTriesNode();
  cTries = createTriesNode();
  populateTriesWithBuildInCommand(tries);
  populateExecutablesIntoTrie(tries);
  editCustomDirectoryIntoTrie(tries, ".", 1);
  enableRawMode();
  jobList.head = NULL;
  jobList.tail = NULL;
  jobList.size = 0;
  jobList.jobPtr = 0;
  historyList.head = NULL;
  historyList.size = 0;
  historyCursor = NULL;
  loadHistory();
  historyAppendCount = historyList.size;
  while (true) {
    setbuf(stdout, NULL);
    printf("$ ");

    char rawCmdStr[1024];
    readCommand(rawCmdStr);
    int rawCmdStrLen = strlen(rawCmdStr);
    addHistory(rawCmdStr);
    Command *CommandArray[64];
    int CommandArrayLen = 0;
    int start = 0;

    for (int i = 0; i <= rawCmdStrLen; i++) {
      if (rawCmdStr[i] == '|' || rawCmdStr[i] == '\0') {
        int diff = i - start;
        char *partialCmdStr = malloc(diff + 1);
        strncpy(partialCmdStr, rawCmdStr + start, diff);
        partialCmdStr[diff] = '\0';
        CommandArray[CommandArrayLen++] = parser(partialCmdStr);
        start = i + 1;
      }
    }

    if (CommandArrayLen == 0) {
      continue;
    }

    char *buffer = NULL;

    if (CommandArrayLen > 1) {
      handlePipe(CommandArray, CommandArrayLen);
    } else {
      if (strcmp(CommandArray[0]->argv[0], "exit") == 0) {
        writeHistory();
        break;
      }
      buffer = cmdHandler(CommandArray[0]);
    }

    if (buffer) {
      printf("%s\n", buffer);
      free(buffer);
    }
    char *job = getDoneJob();
    if (job != NULL) {
      printf("%s\n", job);
      free(job);
    }
  }
  return 0;
}