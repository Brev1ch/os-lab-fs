/* Лабораторная 7 "Файловая система", уровень 1-2-3 (A):
 * два процесса заносят очередной элемент в связанный список, который хранится в ФАЙЛЕ.
 *
 * Формат файла:  [заголовок header][узел][узел]...
 *   header: head - смещение первого узла, tail - смещение последнего, count - число узлов
 *   узел:   pid  - кто добавил, seq - его порядковый номер, next - смещение следующего узла (0 = конец)
 * Вместо указателей в файле хранятся СМЕЩЕНИЯ (off_t) от начала файла.
 *
 * Режимы:
 *   ./filelist init  FILE              создать пустой список
 *   ./filelist add   FILE N [nolock]   добавить N элементов (с блокировкой fcntl или без неё)
 *   ./filelist print FILE              обойти список по ссылкам, проверить целостность
 *   ./filelist demo  FILE N [nolock]   init + два процесса (fork) по N элементов + print
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

struct header { off_t head; off_t tail; long count; };
struct node   { pid_t pid; int seq; off_t next; };

#define HDR_SIZE  ((off_t)sizeof(struct header))

/* ---------- блокировка всего файла (записи) через fcntl ---------- */
static int lock_file(int fd, short type)           /* type: F_WRLCK или F_UNLCK */
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type   = type;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;                                /* 0 = до конца файла (весь файл) */
    return fcntl(fd, F_SETLKW, &fl);                /* W = ждать, пока другой процесс освободит */
}

static int init_list(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return -1; }
    struct header h = { 0, 0, 0 };
    if (pwrite(fd, &h, sizeof h, 0) != sizeof h) { perror("pwrite"); close(fd); return -1; }
    close(fd);
    return 0;
}

/* Добавить один узел в конец списка. */
static int append_node(int fd, int seq, int use_lock)
{
    struct header h;
    struct node n = { getpid(), seq, 0 };

    if (use_lock && lock_file(fd, F_WRLCK) < 0) { perror("lock"); return -1; }

    if (pread(fd, &h, sizeof h, 0) != sizeof h) { perror("pread header"); return -1; }

    usleep(1000);       /* имитация "работы": расширяет окно гонки, если блокировки нет */

    off_t pos = lseek(fd, 0, SEEK_END);            /* новый узел пишем в конец файла */
    if (pwrite(fd, &n, sizeof n, pos) != sizeof n) { perror("pwrite node"); return -1; }

    if (h.tail != 0) {                             /* старый последний узел -> указывает на новый */
        off_t next_field = h.tail + (off_t)((char *)&n.next - (char *)&n);
        if (pwrite(fd, &pos, sizeof pos, next_field) != sizeof pos) { perror("pwrite next"); return -1; }
    } else {
        h.head = pos;                              /* список был пуст */
    }
    h.tail = pos;
    h.count++;
    if (pwrite(fd, &h, sizeof h, 0) != sizeof h) { perror("pwrite header"); return -1; }

    if (use_lock && lock_file(fd, F_UNLCK) < 0) { perror("unlock"); return -1; }
    return 0;
}

static int add_items(const char *path, int count, int use_lock)
{
    int fd = open(path, O_RDWR);                   /* каждый процесс открывает файл САМ */
    if (fd < 0) { perror("open"); return -1; }
    for (int i = 1; i <= count; i++)
    {
        if (append_node(fd, i, use_lock) < 0) { close(fd); return -1; }
        usleep(500);    /* пауза вне блокировки: даём второму процессу шанс вклиниться */
    }
    close(fd);
    return 0;
}

/* Обход списка по ссылкам + сравнение с реальным размером файла. Возвращает 0, если всё цело. */
static int print_list(const char *path, int quiet)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return -1; }
    struct header h;
    if (pread(fd, &h, sizeof h, 0) != sizeof h) { perror("pread"); close(fd); return -1; }

    off_t size = lseek(fd, 0, SEEK_END);
    long in_file = (size - HDR_SIZE) / (off_t)sizeof(struct node);   /* сколько узлов физически в файле */
    long reach = 0;
    if (!quiet) printf("%-6s %-8s %-6s %s\n", "№", "pid", "seq", "смещение");
    for (off_t p = h.head; p != 0 && reach <= in_file; ) {
        struct node n;
        if (pread(fd, &n, sizeof n, p) != sizeof n) break;
        reach++;
        if (!quiet) printf("%-6ld %-8d %-6d %ld\n", reach, (int)n.pid, n.seq, (long)p);
        p = n.next;
    }
    close(fd);
    printf("В заголовке count=%ld, узлов в файле=%ld, достижимых по ссылкам=%ld\n", h.count, in_file, reach);
    if (reach == in_file && h.count == in_file) { printf("РЕЗУЛЬТАТ: список целый\n"); return 0; }
    printf("РЕЗУЛЬТАТ: список ПОВРЕЖДЁН (потеряны элементы)\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Использование:\n  %s init FILE\n  %s add FILE N [nolock]\n  %s print FILE\n  %s demo FILE N [nolock]\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    const char *cmd = argv[1], *path = argv[2];
    int n = argc > 3 ? atoi(argv[3]) : 0;
    int use_lock = !(argc > 4 && strcmp(argv[4], "nolock") == 0);

    if (!strcmp(cmd, "init"))  return init_list(path) ? 1 : 0;
    if (!strcmp(cmd, "add"))   return add_items(path, n, use_lock) ? 1 : 0;
    if (!strcmp(cmd, "print")) return print_list(path, 0);

    if (!strcmp(cmd, "demo")) {
        if (n <= 0) n = 10;
        if (init_list(path)) return 1;
        printf("Два процесса по %d элементов, блокировка: %s\n", n, use_lock ? "ДА (fcntl)" : "НЕТ");
        for (int k = 0; k < 2; k++) {
            pid_t p = fork();
            if (p == 0) _exit(add_items(path, n, use_lock) ? 1 : 0);
            if (p < 0) { perror("fork"); return 1; }
        }
        while (wait(NULL) > 0) ;
        return print_list(path, n > 20);
    }
    fprintf(stderr, "Неизвестная команда\n");
    return 2;
}
