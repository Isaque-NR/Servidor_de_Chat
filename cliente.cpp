// Comando para compilar e executar: g++ -std=c++20 -pthread cliente.cpp -o cliente && ./cliente 
#include <array>
#include<csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <mutex>

#define BUFFER_LENGTH 4096
#define MAX_NAME_LENGTH 40

bool rodando = true;
std::mutex mutex_rodando;

 // verifica se o cliente ainda esta rodando
bool esta_rodando() {
    // Trava o mutex praF acessar a variavel rodando com segurando para impedir que mais de uma thread leia ou altere ela
    std::lock_guard<std::mutex> lock(mutex_rodando);
    return rodando;
}

// altera o estado "rodando" do cliente para parar sua execução 
void parar_cliente() {
    std::lock_guard<std::mutex> lock(mutex_rodando); // trava mutex, mesmo esquema ali de cima
    rodando = false;
}

// funcao pra interpretar a mensagem de acordo com o protocolo solicitado: bom|msg_servidor|conteudo|eom
bool interpretar_mensagem_servidor(const std::string& entrada, std::string& conteudo) {
    std::size_t pos1 = entrada.find('|');
    if (pos1 == std::string::npos) { 
        return false;
    }

    std::size_t pos2 = entrada.find('|', pos1 + 1);
    if (pos2 == std::string::npos) {
        return false;
    }

    std::size_t pos3 = entrada.find('|', pos2 + 1);
    if (pos3 == std::string::npos) {
        return false;
    }

    std::string parte1 = entrada.substr(0, pos1);
    std::string parte2 = entrada.substr(pos1 + 1, pos2 - pos1 - 1);
    std::string parte3 = entrada.substr(pos2 + 1, pos3 - pos2 - 1);
    std::string parte4 = entrada.substr(pos3 + 1);

    if (parte1 != "bom") {
        return false;
    }

    if (parte2 != "msg_servidor") {
        return false;
    }

    if (parte4 != "eom") {
        return false;
    }

    conteudo = parte3;
    return true;
}

// funcao no mesmo esquema no servidor onde extraimos as mensagens acumuladas recebidas pelo revc() 
bool extrair_pacote(std::string& acumulado, std::string& pacote) {
    const std::string fim = "|eom";

    std::size_t pos = acumulado.find(fim);

    if (pos == std::string::npos) {
        return false;
    }

    std::size_t fim_pacote = pos + fim.size();

    pacote = acumulado.substr(0, fim_pacote);
    acumulado.erase(0, fim_pacote);

    return true;
}

// funcao pra montar o pacore pro servidor seguindo o protocolo exigido
void enviar_pacote(int sockfd, const std::string& comando, const std::string& conteudo) {
    std::string pacote = "bom|" + comando + "|" + conteudo + "|eom";

    if (pacote.size() >= BUFFER_LENGTH) {
        pacote.resize(BUFFER_LENGTH - 1);
    }

    send(sockfd, pacote.c_str(), pacote.size(), 0);
}

// funcao da thread recebedora do cliente rodando em paralelo pra receber mensagens vindas do servidor e mostrar na tela
void thread_recebedora(int sockfd) {
    std::array<char, BUFFER_LENGTH> buffer{}; // buffer para armazenar os dados recebidos 
    std::string acumulado;

    while (esta_rodando()) { // enquanto tiver rodando o cliente continua recebendo mensagens 
        buffer.fill('\0'); // preechendo com /0 pra evitar lixo de memoria 

        ssize_t message_len = recv(sockfd, buffer.data(), buffer.size() - 1, 0); // recebe os dados enviados pelo servidor 

        if (message_len <= 0) {
            std::cout << "\nConexao com o servidor encerrada.\n";
            parar_cliente();
            break;
        }

        acumulado.append(buffer.data(), static_cast<std::size_t>(message_len)); //acumulado recebe exatamente os bytes recebidos 

        std::string pacote;

        while (extrair_pacote(acumulado, pacote)) { // enquanto tiver ao menos um pacote completo no acumulado extrai e precessa cada um seperandamente
            std::string conteudo;

            if (interpretar_mensagem_servidor(pacote, conteudo)) { //pacote estando no formato completo extrai o conteudo
                std::cout << "\n" << conteudo << "\n";
                std::cout << "> " << std::flush;
            }
        }
    }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    int sockfd;
    sockaddr_in server{};

    std::string ip_server;
    std::string porta_server;
    std::string nome;
    std::string mensagem;

    std::cout << "Digite o IP do servidor: ";
    std::getline(std::cin, ip_server);

    std::cout << "Digite a porta do servidor: ";
    std::getline(std::cin, porta_server);

    int num_porta = std::atoi(porta_server.c_str());

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1) {
        std::perror("Houve um erro ao criar socket.");
        return EXIT_FAILURE;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(num_porta);

    if (inet_pton(AF_INET, ip_server.c_str(), &server.sin_addr) <= 0) {
        std::perror("Endereco IP invalido");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1) {
        std::perror("Erro ao conectar ao servidor");
        close(sockfd);
        return EXIT_FAILURE;
    }

    std::array<char, BUFFER_LENGTH> buffer{}; // buffer usado pra receber a mensagem inicial do servidor 
    ssize_t message_len = recv(sockfd, buffer.data(), buffer.size() - 1, 0);

    if (message_len <= 0) {
        std::cout << "Conexao encerrada antes da mensagem de boas-vindas.\n";
        close(sockfd);
        return EXIT_FAILURE;
    }
    // Garante que o buffer termine com '\0' para transformar os dados recebidos em uma string válida em C.
    buffer[static_cast<std::size_t>(message_len)] = '\0';

    std::string conteudo;

    if (interpretar_mensagem_servidor(buffer.data(), conteudo)) {
        std::cout << conteudo << "\n";
    }

    std::cout << "Digite seu nome de usuário: ";
    std::getline(std::cin, nome);

    if (nome.size() >= MAX_NAME_LENGTH) {
        nome.resize(MAX_NAME_LENGTH - 1);
    }

    enviar_pacote(sockfd, "usuario_entra", nome); // enviando ao servidor que o usuario entrou

    std::thread tid_recebedora;

    try {
        // cria a thread recebedora executando a funcao thread_recebedora em paralelo
        tid_recebedora = std::thread(thread_recebedora, sockfd);
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "Erro ao criar thread recebedora: %s\n", e.what());
        close(sockfd);
        return EXIT_FAILURE;
    }

    while (esta_rodando()) { // loop principal de envio de mensagens enquanto cliente estiver roda le o teclado
        std::cout << "> " << std::flush;

        if (!std::getline(std::cin, mensagem)) {
            break;
        }

        if (mensagem.empty()) {
            continue;
        }

        if (mensagem == "tchau") { // comando exigido para o cliente sair servidor 
            enviar_pacote(sockfd, "usuario_sai", nome); // envia o pacote ao servidor informando que o usuario desconectou em formato do protocolo
            parar_cliente(); 
            break;
        }

        enviar_pacote(sockfd, "msg_cliente", mensagem); // mensagem comum envia ao servidor
    }

    shutdown(sockfd, SHUT_RDWR);

    if (tid_recebedora.joinable()) {
        tid_recebedora.join();
    }

    close(sockfd); 

    std::cout << "Cliente encerrado.\n";

    return EXIT_SUCCESS;
}