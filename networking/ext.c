#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/nsproxy.h>
#include "../kapi.h"
#include "../vm.h"

typedef uint32_t wasm_pointer_t;

struct import_resolver *resolver;

int __net_socket(
    struct vmctx *ctx,
    int family,
    int type,
    int proto
) {
    struct socket *sock;
    struct file *f;
    int err, fd;

    if((err = sock_create_kern(
        current->nsproxy->net_ns,
        family,
        type,
        proto,
        &sock
    )) < 0) {
        return err;
    }

    fd = get_unused_fd_flags(O_RDWR);
    if(fd < 0) {
        return fd;
    }

    f = sock_alloc_file(sock, O_RDWR, NULL);
    if(IS_ERR(f)) {
        put_unused_fd(fd);
        sock_release(sock);
        return PTR_ERR(f);
    }

    fd_install(fd, f);
    return fd;
}

int __net_bind(
    struct vmctx *ctx,
    int fd,
    wasm_pointer_t sockaddr,
    uint32_t sockaddr_len
) {
    int err;
    struct file *file;
    struct socket *sock;
    struct sockaddr *sa;

    file = fget(fd);
    if(!file) {
        return -EBADF;
    }

    sock = sock_from_file(file, &err);
    if(!sock) {
        fput(file);
        return -ENOTSOCK;
    }

    if(sockaddr) {
        sa = (void *) vmctx_get_memory_slice(ctx, sockaddr, sockaddr_len);
        if(!sa) {
            fput(file);
            return -EFAULT;
        }
    } else {
        if(sockaddr_len != 0) {
            fput(file);
            return -EINVAL;
        }
        sa = NULL;
    }

    err = sock->ops->bind(sock, sa, sockaddr_len);
    fput(file);
    return err;
}

int __net_listen(
    struct vmctx *ctx,
    int fd,
    int backlog
) {
    int err;
    struct file *file;
    struct socket *sock;

    file = fget(fd);
    if(!file) {
        return -EBADF;
    }

    sock = sock_from_file(file, &err);
    if(!sock) {
        fput(file);
        return -ENOTSOCK;
    }

    err = sock->ops->listen(sock, backlog);
    fput(file);
    return err;
}

int __net_accept(
    struct vmctx *ctx,
    int fd,
    wasm_pointer_t sockaddr,
    wasm_pointer_t sockaddr_len_vptr
) {
    int err, new_fd;
    struct file *file, *new_file;
    struct socket *sock, *new_sock;
    struct sockaddr *sa;
    int *sockaddr_len_p;

    file = fget(fd);
    if(!file) {
        return -EBADF;
    }

    sock = sock_from_file(file, &err);
    if(!sock) {
        fput(file);
        return -ENOTSOCK;
    }

    if(sockaddr) {
        sockaddr_len_p = (void *) vmctx_get_memory_slice(ctx, sockaddr_len_vptr, sizeof(int));
        if(!sockaddr_len_p) {
            fput(file);
            return -EFAULT;
        }

        sa = (void *) vmctx_get_memory_slice(ctx, sockaddr, *sockaddr_len_p);
        if(!sa) {
            fput(file);
            return -EFAULT;
        }
    } else {
        sa = NULL;
    }

    if((err = sock_create_lite(AF_INET, SOCK_STREAM, 0, &new_sock)) < 0) {
        fput(file);
        return err;
    }

    new_sock->ops = sock->ops;

    if((err = sock->ops->accept(sock, new_sock, 0, 1)) < 0) {
        sock_release(new_sock);
        fput(file);
        return err;
    }

    fput(file);

    if(sa) {
        if((err = new_sock->ops->getname(new_sock, sa, sockaddr_len_p, 2)) < 0) {
            sock_release(new_sock);
            return err;
        }
    }

    new_file = sock_alloc_file(new_sock, O_RDWR, NULL);
    if(IS_ERR(new_file)) {
        sock_release(new_sock);
        return PTR_ERR(new_file);
    }

    new_fd = get_unused_fd_flags(O_RDWR);
    if(new_fd < 0) {
        fput(new_file);
        return new_fd;
    }
    fd_install(new_fd, new_file);
    return new_fd;
}

int do_resolve(struct import_resolver_instance *self, const char *name, struct import_info *out) {
    if(strcmp(name, "net##_socket") == 0) {
        out->fn = __net_socket;
        out->param_count = 3;
        return 0;
    } else if(strcmp(name, "net##_bind") == 0) {
        out->fn = __net_bind;
        out->param_count = 3;
        return 0;
    } else if(strcmp(name, "net##_listen") == 0) {
        out->fn = __net_listen;
        out->param_count = 2;
        return 0;
    } else if(strcmp(name, "net##_accept") == 0) {
        out->fn = __net_accept;
        out->param_count = 3;
        return 0;
    } else {
        return -EINVAL;
    }
}

int get_instance(struct execution_engine *ee, struct import_resolver *self, struct import_resolver_instance *out) {
    out->resolve = do_resolve;
    return 0;
}

int __init init_module(void) {
    struct import_resolver tmp = {
        .get_instance = get_instance
    };
    resolver = kwasm_resolver_register(&tmp);
    if(IS_ERR(resolver)) {
        return PTR_ERR(resolver);
    }
    return 0;
}

void __exit cleanup_module(void) {
    kwasm_resolver_deregister(resolver);
}

MODULE_LICENSE("GPL");
