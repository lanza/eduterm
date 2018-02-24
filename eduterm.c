#define _XOPEN_SOURCE 600
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* Font selection has not been implemented. Instead, we assume that we
 * get some font by default that fits in 8x16 pixels and has an ascent
 * of 13 pixels -- or something close. */
#define FONT_W 8
#define FONT_H 16
#define FONT_ASC 13

/* Launching /bin/sh may launch a GNU Bash and that can have nasty side
 * effects. On my system, it clobbers ~/.bash_history because it doesn't
 * respect $HISTSIZE from my ~/.bashrc. That's very annoying. So, launch
 * /bin/dash which does nothing of the sort. */
#define SHELL "/bin/dash"

struct PTY
{
    int master, slave;
};

struct X11
{
    int fd;
    Display *dpy;
    int screen;
    Window root;

    Window termwin;
    GC termgc;
    int w, h;

    char *buf;
    int buf_w, buf_h;
    int buf_x, buf_y;
};

bool
pt_pair(struct PTY *pty)
{
    char *slave_name;

    /* Opens the PTY master device. This is the file descriptor that
     * we're reading from and writing to in our terminal emulator. */
    pty->master = posix_openpt(O_RDWR);
    if (pty->master == -1)
    {
        perror("posix_openpt");
        return false;
    }

    /* grantpt() and unlockpt() are housekeeping functions that have to
     * be called before we can open the slave FD. Refer to the manpages
     * on what they do. */
    if (grantpt(pty->master) == -1)
    {
        perror("grantpt");
        return false;
    }

    if (unlockpt(pty->master) == -1)
    {
        perror("grantpt");
        return false;
    }

    /* Up until now, we only have the master FD. We also need a file
     * descriptor for our child process. We get it by asking for the
     * actual path in /dev/pts which we then open using a regular
     * open(). So, unlike pipe(), you don't get two corresponding file
     * descriptors in one go. */

    slave_name = ptsname(pty->master);
    if (slave_name == NULL)
    {
        perror("ptsname");
        return false;
    }

    pty->slave = open(slave_name, O_RDWR);
    if (pty->slave == -1)
    {
        perror("open(slave_name)");
        return false;
    }

    return true;
}

void
x11_key(XKeyEvent *ev, struct PTY *pty)
{
    char buf[32];
    int i, num;
    KeySym ksym;

    num = XLookupString(ev, buf, sizeof buf, &ksym, 0);
    for (i = 0; i < num; i++)
        write(pty->master, &buf[i], 1);
}

void
x11_redraw(struct X11 *x11)
{
    int x, y;
    char buf[1];

    XSetForeground(x11->dpy, x11->termgc, 0);
    XFillRectangle(x11->dpy, x11->termwin, x11->termgc, 0, 0, x11->w, x11->h);

    XSetForeground(x11->dpy, x11->termgc, 0xFFFFFFFF);
    for (y = 0; y < x11->buf_h; y++)
    {
        for (x = 0; x < x11->buf_w; x++)
        {
            buf[0] = x11->buf[y * x11->buf_w + x];
            if (!iscntrl(buf[0]))
            {
                XDrawString(x11->dpy, x11->termwin, x11->termgc,
                            x * FONT_W, y * FONT_H + FONT_ASC,
                            buf, 1);
            }
        }
    }

    XSetForeground(x11->dpy, x11->termgc, 0xFF00FF00);
    XFillRectangle(x11->dpy, x11->termwin, x11->termgc,
                   x11->buf_x * FONT_W,
                   x11->buf_y * FONT_H,
                   FONT_W, FONT_H);

    XSync(x11->dpy, False);
}

bool
x11_setup(struct X11 *x11)
{
    XSetWindowAttributes wa = {
        .background_pixmap = ParentRelative,
        .event_mask = KeyPressMask | KeyReleaseMask | ExposureMask,
    };

    x11->dpy = XOpenDisplay(NULL);
    if (x11->dpy == NULL)
    {
        fprintf(stderr, "Cannot open display\n");
        return false;
    }

    x11->screen = DefaultScreen(x11->dpy);
    x11->root = RootWindow(x11->dpy, x11->screen);
    x11->fd = ConnectionNumber(x11->dpy);

    /* The terminal will have a fixed size of 80x25 cells. This is an
     * arbitrary number. No resizing has been implemented and child
     * processes can't even ask us for the current size (for now).
     *
     * buf_x, buf_y will be the current cursor position. */
    x11->buf_w = 80;
    x11->buf_h = 25;
    x11->buf_x = 0;
    x11->buf_y = 0;
    x11->buf = calloc(x11->buf_w * x11->buf_h, 1);
    if (x11->buf == NULL)
    {
        perror("calloc");
        return false;
    }

    x11->w = x11->buf_w * FONT_W;
    x11->h = x11->buf_h * FONT_H;

    x11->termwin = XCreateWindow(x11->dpy, x11->root,
                                 0, 0,
                                 x11->w, x11->h,
                                 0,
                                 DefaultDepth(x11->dpy, x11->screen),
                                 CopyFromParent,
                                 DefaultVisual(x11->dpy, x11->screen),
                                 CWBackPixmap | CWEventMask,
                                 &wa);
    XMapWindow(x11->dpy, x11->termwin);
    x11->termgc = XCreateGC(x11->dpy, x11->termwin, 0, NULL);

    XSync(x11->dpy, False);

    return true;
}

bool
spawn(struct PTY *pty)
{
    pid_t p;
    char *env[] = { NULL };

    p = fork();
    if (p == 0)
    {
        close(pty->master);

        dup2(pty->slave, 0);
        dup2(pty->slave, 1);
        dup2(pty->slave, 2);
        close(pty->slave);

        execle(SHELL, "-" SHELL, NULL, env);
        return false;
    }
    else if (p > 0)
    {
        close(pty->slave);
        return true;
    }

    perror("fork");
    return false;
}

int
run(struct PTY *pty, struct X11 *x11)
{
    int i, maxfd;
    fd_set readable;
    XEvent ev;
    char buf[1];

    maxfd = pty->master > x11->fd ? pty->master : x11->fd;

    for (;;)
    {
        FD_ZERO(&readable);
        FD_SET(pty->master, &readable);
        FD_SET(x11->fd, &readable);

        if (select(maxfd + 1, &readable, NULL, NULL, NULL) == -1)
        {
            perror("select");
            return 1;
        }

        if (FD_ISSET(pty->master, &readable))
        {
            if (read(pty->master, buf, 1) <= 0)
            {
                /* This is not necessarily an error but also happens
                 * when the child exits normally. */
                fprintf(stderr, "Nothing to read from child: ");
                perror(NULL);
                return 1;
            }

            if (buf[0] == '\n')
            {
                /* On newline characters, we advance to the next line
                 * and clear any previous contents.
                 *
                 * If we reached the bottom of our buffer, we jump back
                 * to the very first line. Proper scrolling may get
                 * implemented later. */
                x11->buf_x = 0;
                x11->buf_y++;
                x11->buf_y %= x11->buf_h;
                for (i = 0; i < x11->buf_w; i++)
                    x11->buf[x11->buf_y * x11->buf_w + i] = 0;
            }
            else
            {
                /* If this is a regular character, advance one cell to
                 * the right. Once we reaced the right-most cell, jump
                 * back to the first one and overwrite existing data. */
                x11->buf[x11->buf_y * x11->buf_w + x11->buf_x] = buf[0];
                x11->buf_x++;
                x11->buf_x %= x11->buf_w;
            }
            x11_redraw(x11);
        }

        if (FD_ISSET(x11->fd, &readable))
        {
            while (XPending(x11->dpy))
            {
                XNextEvent(x11->dpy, &ev);
                switch (ev.type)
                {
                    case Expose:
                        x11_redraw(x11);
                        break;
                    case KeyPress:
                        x11_key(&ev.xkey, pty);
                        break;
                }
            }
        }
    }

    return 0;
}

int
main()
{
    struct PTY pty;
    struct X11 x11;

    if (!x11_setup(&x11))
        return 1;

    if (!pt_pair(&pty))
        return 1;

    if (!spawn(&pty))
        return 1;

    return run(&pty, &x11);
}