#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <windows.h> 
#include <random>
#include <vector>
#include <iostream>
#include <fstream>

// Информация о госте которая передается админу и редактируется им
struct request {
    int id;
    bool hasNumber;
    int money;
    int floor;
    int number;
};

std::ofstream out;
// Размер буфера реквестов, всего в отеле может быть 25 гостей, поэтому взял с небольшим запасом, 
// но он вообще цикличный и я проверяю, что переполнения быть не должно
const int requestSize = 30;
// Цены номера на каждом этаже отеля
const int prices[]{ 2000, 4000, 6000 };
std::vector<request*> requests(requestSize); //буфер реквестов гостей
int front = 0; //индекс для чтения из буфера
int rear = 0; //индекс для записи в буфер
int count = 0; //количество занятых ячеек буфера

pthread_mutex_t mutex; // мьютекс для условных переменных

// поток гостей блокируется при заполнении буфера (редко или вообще не бывает),
// но также используется, чтобы проверить, что запрос гостя уже обработали
pthread_cond_t not_full;

// поток админа блокируется этой условной переменной, 
// когда количество занятых ячеек становится равно 0
pthread_cond_t not_empty;


// параметры гостя
struct guest_params {
    std::string file;
    int id;
    int money;
};

// параметры админа
struct admin_params {
    std::string file;
};

// Функция потока гостей
void* Guest(void* param) {
    std::string file = ((guest_params*)param)->file;
    int id = ((guest_params*)param)->id;
    int money = ((guest_params*)param)->money;
    auto status = new request{ id, false, money, -1, -1 };
    pthread_mutex_lock(&mutex); //защита операции записи
    //заснуть, если достигнуто макс количество реквестов в очереди
    while (count == requestSize) {
        pthread_cond_wait(&not_full, &mutex);
    }
    requests[rear] = status;
    rear = (rear + 1) % requestSize;
    ++count;
    out.open(file, std::ios::app);
    // Гость заходит в отель со своим бюжетом
    out << "Guest " << id << " (current money = " << status->money << "): entered hotel\n";
    out.close();
    printf("Guest %d (current money = %d): entered hotel\n", id, status->money);
    pthread_mutex_unlock(&mutex);
    pthread_cond_signal(&not_empty);
    pthread_mutex_lock(&mutex);
    // Условие выполняется автоматически, т.к. если гостя разбудят, то это значит, что его запрос уже проверили
    pthread_cond_wait(&not_full, &mutex);
    if (status->hasNumber) {
        while (status->money > 0) {
            // Каждую ночь гость тратит деньги на номер, когда они кончаются, он уходит
            out.open(file, std::ios::app);
            out << "Guest " << id << " (current money = " << status->money << "): spent night in room " << 
                ((status->floor) + 1) << '-' << ((status->number) + 1) << '\n';
            out.close();
            printf("Guest %d (current money = %d): spent night in room %d-%d\n", id, status->money, ((status->floor) + 1), ((status->number) + 1));
            while (count == requestSize) {
                pthread_cond_wait(&not_full, &mutex);
            }
            requests[rear] = status;
            rear = (rear + 1) % requestSize;
            ++count;
            pthread_mutex_unlock(&mutex);
            pthread_cond_signal(&not_empty);
            Sleep(100);
            // Лок поставил здесь, потому что до цикла уже был лок и поэтому не мог его поставить до анлока
            pthread_mutex_lock(&mutex);
            // Условие выполняется автоматически, т.к. если гостя разбудят, то это значит, что его запрос уже проверили
            pthread_cond_wait(&not_full, &mutex);
        }

    }
    // Деньги кончились или он вообще не нашёл номер, значит, уходит
    out.open(file, std::ios::app);
    out << "Guest " << id << ": left hotel\n";
    out.close();
    printf("Guest %d: left hotel\n", id);
    pthread_mutex_unlock(&mutex);
    // Гость ушёл из отеля, поток завершается(уничтожается)
    return NULL;
}

//стартовая функция потока админа
void* Admin(void* param) {
    std::string file = ((admin_params*)param)->file;
    request* guest_status;
    // рандомный генератор
    std::random_device dev;
    std::mt19937 rng(dev());
    // заполения массива номеров отеля
    // каждая строчка - это отдельный этаж
    // на 1 этаже 10 номеров по 2000 УЕ
    // на 2 этаже 10 номеров по 4000 УЕ
    // на 3 этаже 5 номеров по 6000 УЕ
    // Поэтому во всех выводах комнаты гостя я пишу этаж-номер
    std::vector<std::vector<bool>> hotel(3);
    hotel[0] = std::vector<bool>(10, true);
    hotel[1] = std::vector<bool>(10, true);
    hotel[2] = std::vector<bool>(5, true);
    while (1) {
        //извлечь элемент из буфера
        pthread_mutex_lock(&mutex); //защита операции чтения
        //заснуть, если количество занятых ячеек равно нулю
        while (count == 0) {
            pthread_cond_wait(&not_empty, &mutex);
        }
        out.open(file, std::ios::app);
        while (count > 0) {
            //изъятие из общего буфера – начало критической секции
            guest_status = requests[front];
            front = (front + 1) % requestSize; //критическая секция
            count--; //занятая ячейка стала свободной
            if (!guest_status->hasNumber) {
                std::vector<std::pair<int, int>> buffer{};
                for (int floor = 0; floor < 3; ++floor) {
                    int qtty = (floor < 2 ? 10 : 5);
                    for (int num = 0; num < qtty; ++num) {
                        // Ищем свободные номера по карману гостя
                        if (guest_status->money >= prices[floor] && hotel[floor][num]) {
                            buffer.push_back(std::make_pair(floor, num));
                        }
                    }
                }
                if (buffer.size() != 0) {
                    // Выбираем рандомный номер из подходящих
                    std::uniform_int_distribution<std::mt19937::result_type> dist(0, buffer.size() - 1);
                    std::pair<int, int> room = buffer[dist(rng)];
                    guest_status->hasNumber = true;
                    hotel[room.first][room.second] = false;
                    guest_status->floor = room.first;
                    guest_status->number = room.second;
                    out << "Admin: Guest " << guest_status->id << " gets room " <<
                        ((guest_status->floor) + 1) << '-' << ((guest_status->number) + 1) << 
                        " for " << prices[guest_status->floor] << " U\n";
                    printf("Admin: Guest %d gets room %d-%d for %d U\n", guest_status->id, ((guest_status->floor) + 1), 
                        ((guest_status->number) + 1), prices[guest_status->floor]);
                }
                else {
                    // Не нашли свободных номеров
                    out << "Admin: could not find a free room suitable for the Guest " << guest_status->id << " budget\n";
                    printf("Admin: could not find a free room suitable for the Guest %d budget\n", guest_status->id);
                    continue;
                }
            }
            if (guest_status->hasNumber) {
                guest_status->money -= prices[guest_status->floor];
                if (guest_status->money < 0) {
                    // Если деньши кончились, то жилец освобождает номер
                    hotel[guest_status->floor][guest_status->number] = true;
                    out << "Admin: Guest " << guest_status->id << " can no longer afford room " << 
                        ((guest_status->floor) + 1) << '-' << ((guest_status->number) + 1) << ". This room is now free\n";
                    printf("Admin: Guest %d can no longer afford room %d-%d. This room is now free\n", guest_status->id, ((guest_status->floor) + 1),
                        ((guest_status->number) + 1));
                }
            }
        }
        out.close();
        //конец критической секции
        pthread_mutex_unlock(&mutex);
        //разбудить потоки-писатели после получения элемента из буфера
        pthread_cond_broadcast(&not_full);
        //обработать полученный элемент 
        Sleep(70);
    }
    return NULL;
}

int main(int argc, char** argv) {
    // Создать / Открыть и очистить файл
    out.open(argv[1]);
    out.close();
    // Инициализация мутексов и условных перменных
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&not_full, NULL);
    pthread_cond_init(&not_empty, NULL);

    //запуск админа
    pthread_t threadA{};
    int firstt = 1;
    pthread_create(&threadA, NULL, Admin, (void*)(new admin_params{ argv[1] }));

    //запуск производства гостей
    int counter = 0;
    while (true) {
        int id = counter++;
        // рандомный генератор
        std::random_device dev;
        std::mt19937 rng(dev());
        std::uniform_int_distribution<std::mt19937::result_type> dist(0, 20000);
        int money = dist(rng);
        pthread_t threadG{};
        pthread_create(&threadG, NULL, Guest, (void*)(new guest_params { argv[1], id, money }));
        Sleep(50);
    }
    return 0;
}
