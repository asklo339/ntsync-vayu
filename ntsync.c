// SPDX-License-Identifier: GPL-2.0-only
/*
 * ntsync.c - Kernel driver for NT synchronization primitives
 * Copyright (C) 2024 Elizabeth Figura <zfigura@codeweavers.com>
 * Backported to Linux 4.14
 */

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/poll.h>

struct ntsync_sem_args {
	__u32 count;
	__u32 max;
};

struct ntsync_mutex_args {
	__u32 count;
	__u32 owner;
};

struct ntsync_event_args {
	__u32 manual;
	__u32 signaled;
};

struct ntsync_wait_args {
	__u64 objs;
	__u32 count;
	__u32 timeout;
	__u32 flags;
	__u32 pad;
	__u32 owner;
	__u32 alert;
	__u32 index;
};

#define NTSYNC_NAME "ntsync"

MODULE_AUTHOR("Elizabeth Figura <zfigura@codeweavers.com>");
MODULE_DESCRIPTION("Kernel driver for NT synchronization primitives");
MODULE_LICENSE("GPL");

#define NTSYNC_MAX_WAIT_COUNT 4

enum ntsync_type {
	NTSYNC_TYPE_SEM,
	NTSYNC_TYPE_MUTEX,
	NTSYNC_TYPE_EVENT,
};

struct ntsync_obj {
 spinlock_t lock;
 int dev_locked;
 enum ntsync_type type;
 struct file *file;
 struct ntsync_device *dev;
 union {
  struct {
   __u32 count;
   __u32 max;
  } sem;
  struct {
   __u32 count;
   __u32 owner;
   bool ownerdead;
  } mutex;
  struct {
   bool manual;
   bool signaled;
  } event;
 } u;
 struct list_head any_waiters;
 struct list_head all_waiters;
 atomic_t all_hint;
};

struct ntsync_q_entry {
 struct list_head node;
 struct ntsync_q *q;
 struct ntsync_obj *obj;
 __u32 index;
};

struct ntsync_q {
 struct task_struct *task;
 __u32 owner;
 atomic_t signaled;
 bool all;
 bool ownerdead;
 __u32 count;
 struct ntsync_q_entry entries[];
};

struct ntsync_device {
 struct mutex wait_all_lock;
 struct file *file;
};

static void dev_lock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
 spin_lock(&obj->lock);
 obj->dev_locked = 1;
 spin_unlock(&obj->lock);
}

static void dev_unlock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
 spin_lock(&obj->lock);
 obj->dev_locked = 0;
 spin_unlock(&obj->lock);
}

static void obj_lock(struct ntsync_obj *obj)
{
 struct ntsync_device *dev = obj->dev;

 for (;;) {
  spin_lock(&obj->lock);
  if (likely(!obj->dev_locked))
   break;
  spin_unlock(&obj->lock);
  mutex_lock(&dev->wait_all_lock);
  spin_lock(&obj->lock);
  spin_unlock(&obj->lock);
  mutex_unlock(&dev->wait_all_lock);
 }
}

static void obj_unlock(struct ntsync_obj *obj)
{
 spin_unlock(&obj->lock);
}

static bool ntsync_lock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
 bool all;

 obj_lock(obj);
 all = atomic_read(&obj->all_hint);
 if (unlikely(all)) {
  obj_unlock(obj);
  mutex_lock(&dev->wait_all_lock);
  dev_lock_obj(dev, obj);
 }

 return all;
}

static void ntsync_unlock_obj(struct ntsync_device *dev, struct ntsync_obj *obj, bool all)
{
 if (all) {
  dev_unlock_obj(dev, obj);
  mutex_unlock(&dev->wait_all_lock);
 } else {
  obj_unlock(obj);
 }
}

static bool is_signaled(struct ntsync_obj *obj, __u32 owner)
{
 obj_lock(obj);

 switch (obj->type) {
 case NTSYNC_TYPE_SEM:
  return !!obj->u.sem.count;
 case NTSYNC_TYPE_MUTEX:
  if (obj->u.mutex.owner && obj->u.mutex.owner != owner)
   return false;
  return obj->u.mutex.count < UINT_MAX;
 case NTSYNC_TYPE_EVENT:
  return obj->u.event.signaled;
 }

 return false;
}

static void try_wake_any_sem(struct ntsync_obj *sem)
{
 struct ntsync_q_entry *entry;

 obj_lock(sem);

 list_for_each_entry(entry, &sem->any_waiters, node) {
  struct ntsync_q *q = entry->q;
  int signaled = -1;

  if (!sem->u.sem.count)
   break;
  if (atomic_cmpxchg(&q->signaled, -1, entry->index) == -1) {
   sem->u.sem.count--;
   wake_up_process(q->task);
  }
 }

 obj_unlock(sem);
}

static void try_wake_any_mutex(struct ntsync_obj *mutex)
{
 struct ntsync_q_entry *entry;

 obj_lock(mutex);

 list_for_each_entry(entry, &mutex->any_waiters, node) {
  struct ntsync_q *q = entry->q;
  int signaled = -1;

  if (mutex->u.mutex.count == UINT_MAX)
   break;
  if (mutex->u.mutex.owner && mutex->u.mutex.owner != q->owner)
   continue;
  if (atomic_cmpxchg(&q->signaled, -1, entry->index) == -1) {
   if (mutex->u.mutex.ownerdead)
    q->ownerdead = true;
   mutex->u.mutex.ownerdead = false;
   mutex->u.mutex.count++;
   mutex->u.mutex.owner = q->owner;
   wake_up_process(q->task);
  }
 }

 obj_unlock(mutex);
}

static void try_wake_any_event(struct ntsync_obj *event)
{
 struct ntsync_q_entry *entry;

 obj_lock(event);

 list_for_each_entry(entry, &event->any_waiters, node) {
  struct ntsync_q *q = entry->q;
  int signaled = -1;

  if (!event->u.event.signaled)
   break;
  if (atomic_cmpxchg(&q->signaled, -1, entry->index) == -1) {
   if (!event->u.event.manual)
    event->u.event.signaled = false;
   wake_up_process(q->task);
  }
 }

 obj_unlock(event);
}

static void try_wake_any_obj(struct ntsync_obj *obj)
{
 switch (obj->type) {
 case NTSYNC_TYPE_SEM:
  try_wake_any_sem(obj);
  break;
 case NTSYNC_TYPE_MUTEX:
  try_wake_any_mutex(obj);
  break;
 case NTSYNC_TYPE_EVENT:
  try_wake_any_event(obj);
  break;
 }
}

static int ntsync_sem_release(struct ntsync_obj *sem, void __user *argp)
{
 struct ntsync_device *dev = sem->dev;
 __u32 __user *user_args = argp;
 __u32 prev_count;
 __u32 args;
 bool all;
 int ret;

 if (copy_from_user(&args, argp, sizeof(args)))
  return -EFAULT;

 all = ntsync_lock_obj(dev, sem);
 prev_count = sem->u.sem.count;

 if (sem->u.sem.count + args > sem->u.sem.max)
  ret = -EOVERFLOW;
 else {
  sem->u.sem.count += args;
  ret = 0;
  try_wake_any_sem(sem);
 }

 ntsync_unlock_obj(dev, sem, all);

 if (!ret && put_user(prev_count, user_args))
  ret = -EFAULT;

 return ret;
}

static int ntsync_sem_read(struct ntsync_obj *sem, void __user *argp)
{
 struct ntsync_sem_args __user *user_args = argp;
 struct ntsync_device *dev = sem->dev;
 struct ntsync_sem_args args;
 bool all;

 all = ntsync_lock_obj(dev, sem);
 args.count = sem->u.sem.count;
 args.max = sem->u.sem.max;
 ntsync_unlock_obj(dev, sem, all);

 if (copy_to_user(user_args, &args, sizeof(args)))
  return -EFAULT;

 return 0;
}

static int ntsync_mutex_unlock(struct ntsync_obj *mutex, void __user *argp)
{
 struct ntsync_mutex_args __user *user_args = argp;
 struct ntsync_device *dev = mutex->dev;
 struct ntsync_mutex_args args;
 __u32 prev_count;
 bool all;
 int ret;

 if (copy_from_user(&args, argp, sizeof(args)))
  return -EFAULT;
 if (!args.owner)
  return -EINVAL;

 all = ntsync_lock_obj(dev, mutex);
 prev_count = mutex->u.mutex.count;

 if (mutex->u.mutex.owner != args.owner)
  ret = -EPERM;
 else {
  if (!--mutex->u.mutex.count)
   mutex->u.mutex.owner = 0;
  ret = 0;
  try_wake_any_mutex(mutex);
 }

 ntsync_unlock_obj(dev, mutex, all);

 if (!ret && put_user(prev_count, &user_args->count))
  ret = -EFAULT;

 return ret;
}

static int ntsync_mutex_read(struct ntsync_obj *mutex, void __user *argp)
{
 struct ntsync_mutex_args __user *user_args = argp;
 struct ntsync_device *dev = mutex->dev;
 struct ntsync_mutex_args args;
 bool all;
 int ret;

 all = ntsync_lock_obj(dev, mutex);
 args.count = mutex->u.mutex.count;
 args.owner = mutex->u.mutex.owner;
 ret = mutex->u.mutex.ownerdead ? -EOWNERDEAD : 0;
 ntsync_unlock_obj(dev, mutex, all);

 if (copy_to_user(user_args, &args, sizeof(args)))
  return -EFAULT;

 return ret;
}

static int ntsync_event_set(struct ntsync_obj *event, void __user *argp)
{
 struct ntsync_device *dev = event->dev;
 __u32 prev_state;
 bool all;

 all = ntsync_lock_obj(dev, event);
 prev_state = event->u.event.signaled;
 event->u.event.signaled = true;
 try_wake_any_event(event);
 ntsync_unlock_obj(dev, event, all);

 if (put_user(prev_state, (__u32 __user *)argp))
  return -EFAULT;

 return 0;
}

static int ntsync_event_reset(struct ntsync_obj *event, void __user *argp)
{
 struct ntsync_device *dev = event->dev;
 __u32 prev_state;
 bool all;

 all = ntsync_lock_obj(dev, event);
 prev_state = event->u.event.signaled;
 event->u.event.signaled = false;
 ntsync_unlock_obj(dev, event, all);

 if (put_user(prev_state, (__u32 __user *)argp))
  return -EFAULT;

 return 0;
}

static int ntsync_event_read(struct ntsync_obj *event, void __user *argp)
{
 struct ntsync_event_args __user *user_args = argp;
 struct ntsync_device *dev = event->dev;
 struct ntsync_event_args args;
 bool all;

 all = ntsync_lock_obj(dev, event);
 args.manual = event->u.event.manual;
 args.signaled = event->u.event.signaled;
 ntsync_unlock_obj(dev, event, all);

 if (copy_to_user(user_args, &args, sizeof(args)))
  return -EFAULT;

 return 0;
}

static void ntsync_free_obj(struct ntsync_obj *obj)
{
 fput(obj->dev->file);
 kfree(obj);
}

static int ntsync_obj_release(struct inode *inode, struct file *file)
{
 ntsync_free_obj(file->private_data);
 return 0;
}

#define NTSYNC_IOC_MAGIC 'N'

#define NTSYNC_IOC_SEM_RELEASE _IOWR(NTSYNC_IOC_MAGIC, 0, __u32)
#define NTSYNC_IOC_SEM_READ _IOR(NTSYNC_IOC_MAGIC, 1, struct ntsync_sem_args)
#define NTSYNC_IOC_MUTEX_UNLOCK _IOWR(NTSYNC_IOC_MAGIC, 2, struct ntsync_mutex_args)
#define NTSYNC_IOC_MUTEX_READ _IOR(NTSYNC_IOC_MAGIC, 3, struct ntsync_mutex_args)
#define NTSYNC_IOC_EVENT_SET _IOW(NTSYNC_IOC_MAGIC, 4, __u32)
#define NTSYNC_IOC_EVENT_RESET _IOW(NTSYNC_IOC_MAGIC, 5, __u32)
#define NTSYNC_IOC_EVENT_READ _IOR(NTSYNC_IOC_MAGIC, 6, struct ntsync_event_args)

static long ntsync_obj_ioctl(struct file *file, unsigned int cmd,
       unsigned long parm)
{
 struct ntsync_obj *obj = file->private_data;
 void __user *argp = (void __user *)parm;

 switch (cmd) {
 case NTSYNC_IOC_SEM_RELEASE:
  return ntsync_sem_release(obj, argp);
 case NTSYNC_IOC_SEM_READ:
  return ntsync_sem_read(obj, argp);
 case NTSYNC_IOC_MUTEX_UNLOCK:
  return ntsync_mutex_unlock(obj, argp);
 case NTSYNC_IOC_MUTEX_READ:
  return ntsync_mutex_read(obj, argp);
 case NTSYNC_IOC_EVENT_SET:
  return ntsync_event_set(obj, argp);
 case NTSYNC_IOC_EVENT_RESET:
  return ntsync_event_reset(obj, argp);
 case NTSYNC_IOC_EVENT_READ:
  return ntsync_event_read(obj, argp);
 }

 return -ENOIOCTLCMD;
}

static const struct file_operations ntsync_obj_fops = {
 .owner   = THIS_MODULE,
 .release = ntsync_obj_release,
 .unlocked_ioctl = ntsync_obj_ioctl,
};

static struct ntsync_obj *ntsync_alloc_obj(struct ntsync_device *dev,
      enum ntsync_type type)
{
 struct ntsync_obj *obj;

 obj = kzalloc(sizeof(*obj), GFP_KERNEL);
 if (!obj)
  return NULL;

 obj->type = type;
 obj->dev = dev;
 get_file(dev->file);
 spin_lock_init(&obj->lock);
 INIT_LIST_HEAD(&obj->any_waiters);
 INIT_LIST_HEAD(&obj->all_waiters);
 atomic_set(&obj->all_hint, 0);

 return obj;
}

static int ntsync_obj_get_fd(struct ntsync_obj *obj)
{
 struct file *file;
 int fd;

 fd = get_unused_fd_flags(O_CLOEXEC);
 if (fd < 0)
  return fd;

file = anon_inode_getfile("ntsync", &ntsync_obj_fops, obj, O_RDWR);
  if (IS_ERR(file)) {
   put_unused_fd(fd);
   return PTR_ERR(file);
  }

 fd_install(fd, file);
 return fd;
}

static int ntsync_create_sem(struct ntsync_device *dev, void __user *argp)
{
 struct ntsync_sem_args args;
 struct ntsync_obj *sem;
 int fd;

 if (copy_from_user(&args, argp, sizeof(args)))
  return -EFAULT;

 if (args.count > args.max)
  return -EINVAL;

 sem = ntsync_alloc_obj(dev, NTSYNC_TYPE_SEM);
 if (!sem)
  return -ENOMEM;

 sem->u.sem.count = args.count;
 sem->u.sem.max = args.max;
 fd = ntsync_obj_get_fd(sem);
 if (fd < 0)
  ntsync_free_obj(sem);

 return fd;
}

static int ntsync_create_mutex(struct ntsync_device *dev, void __user *argp)
{
 struct ntsync_mutex_args args;
 struct ntsync_obj *mutex;
 int fd;

 if (copy_from_user(&args, argp, sizeof(args)))
  return -EFAULT;

 mutex = ntsync_alloc_obj(dev, NTSYNC_TYPE_MUTEX);
 if (!mutex)
  return -ENOMEM;

 mutex->u.mutex.count = args.count;
 mutex->u.mutex.owner = args.owner;
 fd = ntsync_obj_get_fd(mutex);
 if (fd < 0)
  ntsync_free_obj(mutex);

 return fd;
}

static int ntsync_create_event(struct ntsync_device *dev, void __user *argp)
{
 struct ntsync_event_args args;
 struct ntsync_obj *event;
 int fd;

 if (copy_from_user(&args, argp, sizeof(args)))
  return -EFAULT;

 event = ntsync_alloc_obj(dev, NTSYNC_TYPE_EVENT);
 if (!event)
  return -ENOMEM;

 event->u.event.manual = args.manual;
 event->u.event.signaled = args.signaled;
 fd = ntsync_obj_get_fd(event);
 if (fd < 0)
  ntsync_free_obj(event);

 return fd;
}

static struct ntsync_obj *get_obj(struct ntsync_device *dev, int fd)
{
 struct file *file = fget(fd);
 struct ntsync_obj *obj;

 if (!file)
  return NULL;

 if (file->f_op != &ntsync_obj_fops) {
  fput(file);
  return NULL;
 }

 obj = file->private_data;
 if (obj->dev != dev) {
  fput(file);
  return NULL;
 }

 return obj;
}

static void put_obj(struct ntsync_obj *obj)
{
 fput(obj->file);
}

static int setup_wait(struct ntsync_device *dev,
      const struct ntsync_wait_args *args, bool all,
      struct ntsync_q **ret_q)
{
 int fds[NTSYNC_MAX_WAIT_COUNT + 1];
 const __u32 count = args->count;
 size_t size = count * sizeof(fds[0]);
 struct ntsync_q *q;
 __u32 total_count;
 __u32 i, j;

 if (count > NTSYNC_MAX_WAIT_COUNT)
  return -EINVAL;

 total_count = count;

 q = kmalloc(size + sizeof(*q), GFP_KERNEL);
 if (!q)
  return -ENOMEM;

 q->task = current;
 q->owner = args->owner;
 atomic_set(&q->signaled, -1);
 q->all = all;
 q->ownerdead = false;
 q->count = count;

 if (copy_from_user(fds, (void __user *)(unsigned long)args->objs, size))
  goto err;

 for (i = 0; i < count; i++) {
  struct ntsync_q_entry *entry = &q->entries[i];
  struct ntsync_obj *obj = get_obj(dev, fds[i]);

  if (!obj)
   goto err;

  if (all) {
   for (j = 0; j < i; j++) {
    if (obj == q->entries[j].obj) {
     put_obj(obj);
     goto err;
    }
   }
  }

  entry->obj = obj;
  entry->q = q;
  entry->index = i;
 }

 *ret_q = 0;
 kfree(q);
 return 0;

err:
 for (j = 0; j < i; j++)
  put_obj(q->entries[j].obj);
 kfree(q);
 return -EINVAL;
}

static int ntsync_wait_any(struct ntsync_device *dev, void __user *argp)
{
 struct ntsync_wait_args args;
 __u32 i, total_count;
 struct ntsync_q *q;
 int signaled;
 int ret;

 if (copy_from_user(&args, argp, sizeof(args)))
  return -EFAULT;

 ret = setup_wait(dev, &args, false, &q);
 if (ret < 0)
  return ret;

 total_count = args.count;

 for (i = 0; i < total_count; i++) {
  struct ntsync_q_entry *entry = &q->entries[i];
  struct ntsync_obj *obj = entry->obj;

  list_add_tail(&entry->node, &obj->any_waiters);
 }

 for (i = 0; i < total_count; i++) {
  struct ntsync_obj *obj = q->entries[i].obj;

  if (atomic_read(&q->signaled) != -1)
   break;
  try_wake_any_obj(obj);
 }

 ret = -ERESTARTSYS;

 set_current_state(TASK_INTERRUPTIBLE);
 while (atomic_read(&q->signaled) == -1) {
  if (signal_pending(current))
   break;
  schedule();
  set_current_state(TASK_INTERRUPTIBLE);
 }
 __set_current_state(TASK_RUNNING);

 for (i = 0; i < total_count; i++) {
  struct ntsync_q_entry *entry = &q->entries[i];
  list_del(&entry->node);
 }

 signaled = atomic_read(&q->signaled);
 ret = signaled;

 for (i = 0; i < total_count; i++)
  put_obj(q->entries[i].obj);

 kfree(q);

 if (ret >= 0 && put_user((__u32)ret, &((struct ntsync_wait_args __user *)argp)->index))
  ret = -EFAULT;

 return ret;
}

static long ntsync_device_ioctl(struct file *file, unsigned int cmd,
       unsigned long parm)
{
 struct ntsync_device *dev = file->private_data;
 void __user *argp = (void __user *)parm;

 switch (cmd) {
 case 0xba:
  return ntsync_create_sem(dev, argp);
 case 0xbb:
  return ntsync_create_mutex(dev, argp);
 case 0xbc:
  return ntsync_create_event(dev, argp);
 case 0xbd:
  return ntsync_wait_any(dev, argp);
 }

 return -ENOIOCTLCMD;
}

static int ntsync_device_release(struct inode *inode, struct file *file)
{
 struct ntsync_device *dev = file->private_data;

 kfree(dev);
 return 0;
}

static int ntsync_device_open(struct inode *inode, struct file *file)
{
 struct ntsync_device *dev;

 dev = kzalloc(sizeof(*dev), GFP_KERNEL);
 if (!dev)
  return -ENOMEM;

 mutex_init(&dev->wait_all_lock);
 dev->file = file;
 file->private_data = dev;

 return 0;
}

static const struct file_operations ntsync_device_fops = {
 .owner   = THIS_MODULE,
 .open    = ntsync_device_open,
 .release = ntsync_device_release,
 .unlocked_ioctl = ntsync_device_ioctl,
};

static struct miscdevice ntsync_miscdev = {
 .minor = MISC_DYNAMIC_MINOR,
 .name  = NTSYNC_NAME,
 .fops  = &ntsync_device_fops,
};

static int __init ntsync_init(void)
{
 int ret;

 ret = misc_register(&ntsync_miscdev);
 if (ret) {
  pr_err("ntsync: cannot register misc device\n");
  return ret;
 }

 pr_info("ntsync: driver loaded\n");
 return 0;
}

static void __exit ntsync_exit(void)
{
 misc_deregister(&ntsync_miscdev);
 pr_info("ntsync: driver unloaded\n");
}

module_init(ntsync_init);
module_exit(ntsync_exit);