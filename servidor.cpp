//Comando para compilar e executar: g++ -std=c++20 -pthread servidor.cpp -o servidor && ./servidor 4242
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <thread>
#include <array>
#include <mutex>
#include <semaphore>
#include <system_error>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_BUFFER 32
#define BUFFER_LENGTH 4096
#define MAX_CLIENTS 32
#define MAX_NAME_LENGTH 40
 
// O buffer compartilhado é um objeto do tipo queue, ou seja, uma fila de strings FIFO
std::queue<std::string> fila_mensagens;
 
// Mutex para garantir que só uma thread mexa no buffer por vez
std::mutex mutex_buffer; 

// Semáforo de espaços vazios, ele começa em MAX_BUFFER, ou seja, tudo vazio com espaço disponivel pra colocar strings
std::counting_semaphore<MAX_BUFFER> sem_vazios(MAX_BUFFER);
 
// Semáforo de itens disponíveis, ele começa em 0 pois não há mensagens para consumir
std::counting_semaphore<MAX_BUFFER> sem_msgDisponiveis(0);
 
struct Cliente{

    // socket do cliente, atravez desse numero que o servidor consegue fazer a comunicação com o cliente, começa em -1 pois ainda não existe um socket válido associado
    int fd{-1}; 
    bool ativo{false}; // variável de controle pra saber se o cliente está conectado ou nao no servidor 
    std::string nome;
};

std::array<Cliente, MAX_CLIENTS> clientes;
std::mutex mutex_clientes;

// Insere uma mensagem no buffer
void buffer_inserir(const std::string& msg) {

    // acontece uma espera até que exista ao menos uma lugar vazio no buffer, se o buffer tiver cheio a thread fica bloqueada aqui
    sem_vazios.acquire(); // equivalente ao down dos semaforos 
    {         
        std::lock_guard<std::mutex> lock(mutex_buffer); // trava mutex 
        fila_mensagens.push(msg); // coloca mensagem no fim da fila
    }
    // nao tem um mutex unclock pq ao sair desse bloco, o lock_guard é destruído automaticamente e o mutex libera

    sem_msgDisponiveis.release(); // equivalente ao up dos semaforos 
}
    
// Retira uma mensagem do buffer
std::string buffer_retirar() {

    // Espera até existir pelo menos uma mensagem disponível no buffer.
    // Se a fila estiver vazia, a thread consumidora fica bloqueada aqui.
    sem_msgDisponiveis.acquire();

    std::string msg;

    {
        // trava o mutex para garantir acesso exclusivo ao buffer, impede que outra thread insira ou remova uma mensagem
        std::lock_guard<std::mutex> lock(mutex_buffer);
        msg = fila_mensagens.front(); // pega primeira mensagem da fila
        fila_mensagens.pop(); // remove a primeira mensagem da fila
    }
    // aqui msm esquema ao sair lock_guard é destruido e libera mutex 
    
    // avisa que agora tem um espaço vazio a mais no buffer e libera outra thread produtora que poderia estar esperando um espaço
    sem_vazios.release();
    return msg;
}

// Adiciona um novo cliente na lista de conectados 
int adicionar_cliente(int clientfd, const std::string& nome) {

    // Trava o mutex para garantir que apenas uma thread mexa na lista de clientes por vez.
    std::lock_guard<std::mutex> lock(mutex_clientes);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clientes[i].ativo) {
            clientes[i].fd = clientfd;
            clientes[i].ativo = true;
            clientes[i].nome = nome;
            return i;
        }
    }
    return -1;
}

// Remove um cliente da lista de conectados
void remover_cliente(int indice) {
        
    // Trava o mutex para garantir que apenas uma thread mexa na lista de clientes por vez.
    std::lock_guard<std::mutex> lock(mutex_clientes);

    if (indice >= 0 && indice < MAX_CLIENTS && clientes[indice].ativo) {
        close(clientes[indice].fd); // fecha o socket e conexao do cliente com o servidor 
        clientes[indice] = Cliente{};  // reseta a posicao do vetor pra um cliente vazio default 
    }
}

// Envia mensagem para todos os clientes conectados 
void enviar_para_todos(const std::string& msg) {
    std::string pacote = "bom|msg_servidor|" + msg + "|eom"; // pacote com o protocolo exigido

    //Trava o mutex para garantir que a lista de clientes não seja alterada enquanto percorre ela.
    std::lock_guard<std::mutex> lock(mutex_clientes);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clientes[i].ativo) {
            send(clientes[i].fd, pacote.c_str(), pacote.size(), 0); 
        }
    }
}

//Interpreta a mensagem recebida pra ver se segue o protocolo 
bool interpretar_mensagem(const char *entrada, char *comando, char *conteudo) {
     if (entrada == nullptr || comando == nullptr || conteudo == nullptr) {
        return false;
    }

    std::string mensagem(entrada);
    
    // divide a mensagem em partes 
    size_t pos1 = mensagem.find('|');
    if (pos1 == std::string::npos) {
        return false;
    }

    size_t pos2 = mensagem.find('|', pos1 + 1);
    if (pos2 == std::string::npos) {
        return false;
    }

    size_t pos3 = mensagem.find('|', pos2 + 1);
    if (pos3 == std::string::npos) {
        return false;
    }

    std::string parte1 = mensagem.substr(0, pos1);
    std::string parte2 = mensagem.substr(pos1 + 1, pos2 - pos1 - 1);
    std::string parte3 = mensagem.substr(pos2 + 1, pos3 - pos2 - 1);
    std::string parte4 = mensagem.substr(pos3 + 1);

    //verifica se está seguindo o padrao do protocolo
     if (parte1 != "bom") {
        return false;
    }

    if (parte4 != "eom") {
        return false;
    }

    std::strncpy(comando, parte2.c_str(), MAX_NAME_LENGTH - 1);
    comando[MAX_NAME_LENGTH - 1] = '\0';

    std::strncpy(conteudo, parte3.c_str(), BUFFER_LENGTH - 1);
    conteudo[BUFFER_LENGTH - 1] = '\0';

    return true;
}

// antes acontecia de por conta do envio do pacote pela rede tcp com recv(), duas mensagens se tornavam apenas uma 
// essa funcao serve pra extrair um pacote completo de dentro da string acumulada 
bool extrair_pacote(std::string& acumulado, std::string& pacote) {
    const std::string fim = "|eom";

    std::size_t pos = acumulado.find(fim);

    if (pos == std::string::npos) {
        return false;
    }

    std::size_t fim_pacote = pos + fim.size();

    //Exemplo:  acumulado = "bom|msg|oi|eombom|msg|teste|eom"
    //          pacote    = "bom|msg|oi|eom"
    pacote = acumulado.substr(0, fim_pacote);
    // remove do acumulado o pacote que foi extraido e continua pra pegar outras mensagens em acumulado se tiver
    acumulado.erase(0, fim_pacote);

    return true;
}

// Thread responsavel por consumir o buffer do servidor e enviar elas pra todos os clientes conectados 
void thread_consumidora() {
    while (true) {
        std::string msg = buffer_retirar();
        enviar_para_todos(msg);
    }
}

// Funcao da thread produtora de cada cliente
// recebe um cliente especifico, interpreta os pacotes e coloca mensagens no buffer para a thread consumidora enviar para todos.
void thread_produtora_cliente(int clientfd) {
    int indice = -1;

    char buffer[BUFFER_LENGTH];
    char comando[MAX_NAME_LENGTH];
    char conteudo[BUFFER_LENGTH];

    std::string nome;
    std::string acumulado;

    bool cliente_registrado = false;

    const char *boas_vindas = "bom|msg_servidor|Olá! Seja bem-vindo ao chat!|eom";
    send(clientfd, boas_vindas, std::strlen(boas_vindas), 0);

    while (true) { // enquanto o cliente estiver conectado a thread recebe mensagens dele
        std::memset(buffer, 0, BUFFER_LENGTH); // limpando o buffer antes de receber uma mensagem nova

        int message_len = recv(clientfd, buffer, BUFFER_LENGTH - 1, 0); // recebe os dados enviados pelo cliente atraves do socket

        if (message_len <= 0) { // se recv() retornou 0 ou valor negativo ocorreu um erro ou desconexao
            if (cliente_registrado) {
                buffer_inserir(nome + " saiu da sala de conversa.");
                remover_cliente(indice);
            } else {
                close(clientfd); // se o cliente n tava registrado apenas fecha o socket
            }

            return; // encerra a thread desse cliente
        }
        // adiciona ao acumulado os bytes recebidos pelo revc() pra depois serem estraidos em mensagnes 
        acumulado.append(buffer, static_cast<std::size_t>(message_len));

        std::string pacote;

        while (extrair_pacote(acumulado, pacote)) { // nesse quile extrai e processa o pacote intrepretando se está valido
            if (!interpretar_mensagem(pacote.c_str(), comando, conteudo)) {
                continue;
            }

            if (!cliente_registrado) { // se o cliente nao ta registrado unico comando que aceita é usuario_entra
                if (std::strcmp(comando, "usuario_entra") == 0) {
                    if (std::strlen(conteudo) == 0) {
                        continue;
                    }

                    nome = conteudo;

                    indice = adicionar_cliente(clientfd, nome);

                    if (indice == -1) { // indice negativo significa que n tem espaco na lista de clientes, servidor cheio
                        const char *msg = "bom|msg_servidor|Servidor cheio.|eom";
                        send(clientfd, msg, std::strlen(msg), 0);
                        close(clientfd);
                        return; // encerra a thread
                    }

                    cliente_registrado = true;

                    buffer_inserir(nome + " entrou na sala de conversa");
                }

                continue;
            }
            
            // a partir daqui o cliente esta registrado e pode enviar mensagns ou sair da sala

            if (std::strcmp(comando, "msg_cliente") == 0) {
                buffer_inserir(nome + " enviou: " + conteudo);

            } else if (std::strcmp(comando, "usuario_sai") == 0) {
                buffer_inserir(nome + " saiu da sala de conversa.");
                remover_cliente(indice);
                return; // encerra a thread desse cliente
            }
        }
    }
}

// funcao que cria o socket, configura a porta, comeca a escutar as conexoes e cria uama thread pra cada cliente conectado
int main(int argc, char *argv[]) { 
    std::signal(SIGPIPE, SIG_IGN);
    
    struct sockaddr_in client, server;
    int serverfd, clientfd;
    int porta;

    if (argc != 2) {
        std::fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        return EXIT_FAILURE;
    }

    porta = std::atoi(argv[1]);

    serverfd = socket(AF_INET, SOCK_STREAM, 0);

    if (serverfd == -1) {
        std::perror("Can't create the server socket:");
        return EXIT_FAILURE;
    }

    std::memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(porta);
    server.sin_addr.s_addr = INADDR_ANY;

    int yes = 1;

    if (setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        std::perror("Socket options error:");
        return EXIT_FAILURE;
    }

    if (bind(serverfd, (struct sockaddr *) &server, sizeof(server)) == -1) {
        std::perror("Socket bind error:");
        return EXIT_FAILURE;
    }

    if (listen(serverfd, MAX_CLIENTS) == -1) {
        std::perror("Listen error:");
        return EXIT_FAILURE;
    }

    std::thread tid_consumidora(thread_consumidora); // cria a thread consumidora para retirar mensagens do budder e enviar 
    tid_consumidora.detach();

    std::printf("Servidor aguardando conexoes na porta %d\n", porta);

    while (true) { // vai aceitando novas conexoes de clientes 
        socklen_t client_len = sizeof(client);

        clientfd = accept(serverfd, (struct sockaddr *) &client, &client_len);

        if (clientfd == -1) {
            std::perror("Accept error:");
            continue;
        }     

        try {
            // aqui cria uma thread produtora pra atender a cada cliente que se conectou 
            std::thread tid_produtora(thread_produtora_cliente, clientfd);
            tid_produtora.detach();// Desvincula a thread produtora da main para que a main pode continuar aceitando novos clientes
        } catch (const std::system_error &e) {
            std::fprintf(stderr, "Erro ao criar thread produtora: %s\n", e.what());
            close(clientfd);
            continue;
        }
    }

    close(serverfd);
    return EXIT_SUCCESS;
}
