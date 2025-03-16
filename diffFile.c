diff --git a/kernel/fs.c b/kernel/fs.c
index 53586d5..4fce2f4 100644
--- a/kernel/fs.c
+++ b/kernel/fs.c
@@ -375,29 +375,71 @@ iunlockput(struct inode *ip)
 
 // Return the disk block address of the nth block in inode ip.
 // If there is no such block, bmap allocates one.
-static uint
+static uint 
 bmap(struct inode *ip, uint bn)
 {
   uint addr, *a;
   struct buf *bp;
 
-  if(bn < NDIRECT){
-    if((addr = ip->addrs[bn]) == 0)
+  if (bn < NDIRECT) {
+    if ((addr = ip->addrs[bn]) == 0)
       ip->addrs[bn] = addr = balloc(ip->dev);
     return addr;
   }
+
   bn -= NDIRECT;
 
-  if(bn < NINDIRECT){
-    // Load indirect block, allocating if necessary.
-    if((addr = ip->addrs[NDIRECT]) == 0)
+  // Handle singly-indirect block
+  if (bn < NINDIRECT) {
+    if ((addr = ip->addrs[NDIRECT]) == 0)
       ip->addrs[NDIRECT] = addr = balloc(ip->dev);
+
     bp = bread(ip->dev, addr);
     a = (uint*)bp->data;
-    if((addr = a[bn]) == 0){
+
+    if ((addr = a[bn]) == 0) {
       a[bn] = addr = balloc(ip->dev);
       log_write(bp);
     }
+
+    brelse(bp);
+    return addr;
+  }
+
+  // Now we are in doubly-indirect block territory
+  bn -= NINDIRECT;
+
+  if (bn < NDINDIRECT) {
+    // Step 1: Allocate/load the doubly-indirect block
+    if ((addr = ip->addrs[NDIRECT + 1]) == 0)
+      ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);
+
+    bp = bread(ip->dev, addr);
+    a = (uint*)bp->data;
+
+    // Step 2: Allocate blocks in the first level up to the index
+    int index = bn / NINDIRECT;
+
+    for (int i = 0; i <= index; i++) {
+      if ((addr = a[i]) == 0) {
+        a[i] = addr = balloc(ip->dev);
+        log_write(bp);  // Log write for each new allocation
+      }
+    }
+
+    brelse(bp);  // Done with first level
+
+    // Step 3: Read the second level indirect block and allocate the data block
+    bn -= NINDIRECT * index;
+
+    bp = bread(ip->dev, addr);
+    a = (uint*)bp->data;
+
+    if ((addr = a[bn]) == 0) {
+      a[bn] = addr = balloc(ip->dev);
+      log_write(bp);
+    }
+
     brelse(bp);
     return addr;
   }
@@ -414,9 +456,10 @@ static void
 itrunc(struct inode *ip)
 {
   int i, j;
-  struct buf *bp;
-  uint *a;
+  struct buf *bp, *bp2;
+  uint *a, *a2;
 
+  // Free direct blocks.
   for(i = 0; i < NDIRECT; i++){
     if(ip->addrs[i]){
       bfree(ip->dev, ip->addrs[i]);
@@ -424,6 +467,7 @@ itrunc(struct inode *ip)
     }
   }
 
+  // Free single-indirect blocks.
   if(ip->addrs[NDIRECT]){
     bp = bread(ip->dev, ip->addrs[NDIRECT]);
     a = (uint*)bp->data;
@@ -436,10 +480,30 @@ itrunc(struct inode *ip)
     ip->addrs[NDIRECT] = 0;
   }
 
+  // Free double-indirect blocks.
+  if(ip->addrs[NDIRECT]){
+    bp = bread(ip->dev, ip->addrs[NDIRECT]);
+    a = (uint*)bp->data;
+    for(i = 0; i < NINDIRECT; i++){
+      if(a[i]){
+        bp2 = bread(ip->dev, a[i]);
+        a2 = (uint*)bp2->data;
+        for(j = 0; j < NINDIRECT; j++){
+          if(a2[j])
+            bfree(ip->dev, a2[j]);
+        }
+        brelse(bp2);
+        bfree(ip->dev, a[i]);
+      }
+    }
+    brelse(bp);
+    bfree(ip->dev, ip->addrs[NDIRECT]);
+    ip->addrs[NDIRECT] = 0;
+  }
+
   ip->size = 0;
   iupdate(ip);
 }
-
 // Copy stat information from inode.
 // Caller must hold ip->lock.
 void
diff --git a/kernel/fs.h b/kernel/fs.h
index 139dcc9..c190a39 100644
--- a/kernel/fs.h
+++ b/kernel/fs.h
@@ -24,9 +24,13 @@ struct superblock {
 
 #define FSMAGIC 0x10203040
 
-#define NDIRECT 12
+#define NDIRECT 11
 #define NINDIRECT (BSIZE / sizeof(uint))
-#define MAXFILE (NDIRECT + NINDIRECT)
+#define NDINDIRECT (NINDIRECT * NINDIRECT)  // Doubly indirect blocks
+
+#define MAXFILE (NDIRECT + NINDIRECT + NDINDIRECT)
+
+
 
 // On-disk inode structure
 struct dinode {
@@ -35,7 +39,7 @@ struct dinode {
   short minor;          // Minor device number (T_DEVICE only)
   short nlink;          // Number of links to inode in file system
   uint size;            // Size of file (bytes)
-  uint addrs[NDIRECT+1];   // Data block addresses
+  uint addrs[NDIRECT+2];   // Data block addresses
 };
 
 // Inodes per block.
@@ -58,3 +62,4 @@ struct dirent {
   char name[DIRSIZ];
 };
 
+struct inode* resolve_symlink(struct inode* ip, int depth);
diff --git a/kernel/sysfile.c b/kernel/sysfile.c
index 9d95ac9..33a5f80 100644
--- a/kernel/sysfile.c
+++ b/kernel/sysfile.c
@@ -184,58 +184,75 @@ isdirempty(struct inode *dp)
 uint64
 sys_unlink(void)
 {
-  struct inode *ip, *dp;
-  struct dirent de;
-  char name[DIRSIZ], path[MAXPATH];
-  uint off;
+    struct inode *ip, *dp;
+    struct dirent de;
+    char name[DIRSIZ], path[MAXPATH];
+    uint off;
+
+    if (argstr(0, path, MAXPATH) < 0)
+        return -1;
+
+    begin_op(ROOTDEV);
+    if ((dp = nameiparent(path, name)) == 0) {
+        end_op(ROOTDEV);
+        return -1;
+    }
 
-  if(argstr(0, path, MAXPATH) < 0)
-    return -1;
+    ilock(dp);
 
-  begin_op(ROOTDEV);
-  if((dp = nameiparent(path, name)) == 0){
-    end_op(ROOTDEV);
-    return -1;
-  }
+    // Cannot unlink "." or "..".
+    if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
+        goto bad;
 
-  ilock(dp);
+    if ((ip = dirlookup(dp, name, &off)) == 0)
+        goto bad;
+    ilock(ip);
 
-  // Cannot unlink "." or "..".
-  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
-    goto bad;
+    if (ip->nlink < 1)
+        panic("unlink: nlink < 1");
 
-  if((ip = dirlookup(dp, name, &off)) == 0)
-    goto bad;
-  ilock(ip);
+    // If it's a directory and not empty, prevent unlinking
+    if (ip->type == T_DIR && !isdirempty(ip)) {
+        iunlockput(ip);
+        goto bad;
+    }
 
-  if(ip->nlink < 1)
-    panic("unlink: nlink < 1");
-  if(ip->type == T_DIR && !isdirempty(ip)){
-    iunlockput(ip);
-    goto bad;
-  }
+    // Handle symlink unlinking
+    if (ip->type == T_SYMLINK) {
+        memset(&de, 0, sizeof(de));
+        if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
+            panic("unlink: writei");
+
+        ip->nlink--;
+        iupdate(ip);
+        iunlockput(ip);
+        iunlockput(dp);
+        end_op(ROOTDEV);
+        return 0;
+    }
 
-  memset(&de, 0, sizeof(de));
-  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
-    panic("unlink: writei");
-  if(ip->type == T_DIR){
-    dp->nlink--;
-    iupdate(dp);
-  }
-  iunlockput(dp);
+    memset(&de, 0, sizeof(de));
+    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
+        panic("unlink: writei");
 
-  ip->nlink--;
-  iupdate(ip);
-  iunlockput(ip);
+    if (ip->type == T_DIR) {
+        dp->nlink--;
+        iupdate(dp);
+    }
 
-  end_op(ROOTDEV);
+    iunlockput(dp);
+    ip->nlink--;
+    iupdate(ip);
+    iunlockput(ip);
 
-  return 0;
+    end_op(ROOTDEV);
+
+    return 0;
 
 bad:
-  iunlockput(dp);
-  end_op(ROOTDEV);
-  return -1;
+    iunlockput(dp);
+    end_op(ROOTDEV);
+    return -1;
 }
 
 static struct inode*
@@ -286,75 +303,182 @@ create(char *path, short type, short major, short minor)
 uint64
 sys_symlink(void)
 {
-  //your implementation goes here
-  return 0;
+    char target[MAXPATH], path[MAXPATH];
+    struct inode *ip;
+
+    // Extract the arguments: target and path
+    if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
+        return -1;
+
+    // Start filesystem operation
+    begin_op(ROOTDEV);
+
+    // Create a new inode of type T_SYMLINK at the given path
+    ip = create(path, T_SYMLINK, 0, 0);
+    if (ip == 0) {
+        end_op(ROOTDEV);
+        return -1;
+    }
+
+    int len = strlen(target);
+
+    // Write the length first
+    if (writei(ip, 0, (uint64)&len, 0, sizeof(int)) != sizeof(int)) {
+        iunlockput(ip);
+        end_op(ROOTDEV);
+        return -1;
+    }
+
+    // Write the target path string after the length
+    if (writei(ip, 0, (uint64)target, sizeof(int), len + 1) != len + 1) {
+        iunlockput(ip);
+        end_op(ROOTDEV);
+        return -1;
+    }
+
+    iupdate(ip);
+    iunlockput(ip);
+
+    // End filesystem operation
+    end_op(ROOTDEV);
+
+    return 0;
 }
 
+
 uint64
 sys_open(void)
 {
-  char path[MAXPATH];
-  int fd, omode;
-  struct file *f;
-  struct inode *ip;
-  int n;
-
-  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
-    return -1;
+    char path[MAXPATH], target[MAXPATH];
+    int fd, omode;
+    struct file *f;
+    struct inode *ip;
+    int n;
+
+    if ((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
+        return -1;
+
+    begin_op(ROOTDEV);
+
+    if (omode & O_CREATE) {
+        ip = create(path, T_FILE, 0, 0);
+        if (ip == 0) {
+            end_op(ROOTDEV);
+            return -1;
+        }
+    } else {
+        ip = namei(path);
+        if (ip == 0) {
+            end_op(ROOTDEV);
+            return -1;
+        }
+
+        ilock(ip);
+
+        // Symlink resolution
+        if ((ip->type == T_SYMLINK) && !(omode & O_NOFOLLOW)) {
+            int count = 0;
+
+            while (ip->type == T_SYMLINK && count < 10) {
+                int len = 0;
+
+                // First read the length of the target path from the inode
+                if (readi(ip, 0, (uint64)&len, 0, sizeof(int)) != sizeof(int)) {
+                    iunlockput(ip);
+                    end_op(ROOTDEV);
+                    return -1;
+                }
+
+                if (len <= 0 || len > MAXPATH) {
+                    printf("open: corrupted symlink inode\n");
+                    iunlockput(ip);
+                    end_op(ROOTDEV);
+                    return -1;
+                }
+
+                // Then read the actual target path
+                if (readi(ip, 0, (uint64)target, sizeof(int), len + 1) != len + 1) {
+                    iunlockput(ip);
+                    end_op(ROOTDEV);
+                    return -1;
+                }
+
+                target[len] = '\0'; // Null-terminate, just to be safe
+
+                iunlockput(ip);
+
+                // Look up the inode of the target path
+                ip = namei(target);
+                if (ip == 0) {
+                    end_op(ROOTDEV);
+                    return -1;
+                }
+
+                ilock(ip);
+                count++;
+            }
+
+            if (count >= 10) {
+                printf("open: symlink cycle detected!\n");
+                iunlockput(ip);
+                end_op(ROOTDEV);
+                return -1;
+            }
+        }
+
+        // Disallow opening directories for writing
+        if (ip->type == T_DIR && omode != O_RDONLY) {
+            iunlockput(ip);
+            end_op(ROOTDEV);
+            return -1;
+        }
+    }
 
-  begin_op(ROOTDEV);
+    // Validate device inode
+    if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
+        iunlockput(ip);
+        end_op(ROOTDEV);
+        return -1;
+    }
 
-  if(omode & O_CREATE){
-    ip = create(path, T_FILE, 0, 0);
-    if(ip == 0){
-      end_op(ROOTDEV);
-      return -1;
+    // Allocate file structure
+    f = filealloc();
+    if (f == 0) {
+        iunlockput(ip);
+        end_op(ROOTDEV);
+        return -1;
     }
-  } else {
-    if((ip = namei(path)) == 0){
-      end_op(ROOTDEV);
-      return -1;
+
+    // Allocate file descriptor
+    fd = fdalloc(f);
+    if (fd < 0) {
+        fileclose(f);
+        iunlockput(ip);
+        end_op(ROOTDEV);
+        return -1;
     }
-    ilock(ip);
-    if(ip->type == T_DIR && omode != O_RDONLY){
-      iunlockput(ip);
-      end_op(ROOTDEV);
-      return -1;
+
+    // Set file struct properties
+    if (ip->type == T_DEVICE) {
+        f->type = FD_DEVICE;
+        f->major = ip->major;
+        f->minor = ip->minor;
+    } else {
+        f->type = FD_INODE;
     }
-  }
 
-  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
-    iunlockput(ip);
-    end_op(ROOTDEV);
-    return -1;
-  }
+    f->ip = ip;
+    f->off = 0;
+    f->readable = !(omode & O_WRONLY);
+    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
 
-  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
-    if(f)
-      fileclose(f);
-    iunlockput(ip);
+    iunlock(ip);
     end_op(ROOTDEV);
-    return -1;
-  }
-
-  if(ip->type == T_DEVICE){
-    f->type = FD_DEVICE;
-    f->major = ip->major;
-    f->minor = ip->minor;
-  } else {
-    f->type = FD_INODE;
-  }
-  f->ip = ip;
-  f->off = 0;
-  f->readable = !(omode & O_WRONLY);
-  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
-
-  iunlock(ip);
-  end_op(ROOTDEV);
 
-  return fd;
+    return fd;
 }
 
+
 uint64
 sys_mkdir(void)
 {
@@ -490,3 +614,20 @@ sys_pipe(void)
   return 0;
 }
 
+
+//struct inode* resolve_symlink(struct inode* ip, int depth) {
+  //  char target[MAXPATH];
+    
+    //if (depth > 10) { // Prevent infinite loops
+      //  iput(ip);
+        //return 0;
+    //}
+
+   // if (ip->type == T_SYMLINK) {
+     //   readi(ip, 0, (uint64)target, 0, MAXPATH);
+       // iput(ip);
+       // return resolve_symlink(namei(target), depth + 1);
+    //}
+    
+   // return ip;
+//}
diff --git a/user/user.h b/user/user.h
index c958382..b7e851d 100644
--- a/user/user.h
+++ b/user/user.h
@@ -16,7 +16,7 @@ int mknod(const char*, short, short);
 int unlink(const char*);
 int fstat(int fd, struct stat*);
 int link(const char*, const char*);
-int symlink(const char*, const char*);
+int symlink(const char *target, const char *path);
 int mkdir(const char*);
 int chdir(const char*);
 int dup(int);
