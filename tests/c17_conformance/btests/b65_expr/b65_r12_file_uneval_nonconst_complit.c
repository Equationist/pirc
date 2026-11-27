/* C17 6.5.2.5p3: file-scope compound literal requires constant initializer even in sizeof */
int x;
int a[sizeof((int){x})];
