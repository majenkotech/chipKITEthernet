#include <chipKITEthernet.h>

TelnetServer::TelnetServer() {
    _port = 23;
    _clients = NULL;
    _connectHandler = NULL;
    _commands = NULL;
}

TelnetServer::TelnetServer(uint16_t port) {
    _port = port;
    _clients = NULL;
    _connectHandler = NULL;
    _commands = NULL;
}

void TelnetServer::begin() {
    _server = new Server(_port);
    _server->begin();
}

void TelnetServer::serve() {
    TelnetClient *scan;
    Client cli = _server->available();

    if (cli) {
        TelnetClient *found = findByClient(&cli);
        if (found == NULL) {
            addClient(&cli);
        }
    }

    for (scan = _clients; scan; scan = scan->next) {
        serveClient(scan);
    }
}

TelnetClient *TelnetServer::findByClient(Client *c) {
    TelnetClient *scan;
    if (_clients == NULL) {
        return NULL;
    }
    for (scan = _clients; scan; scan = scan->next) {
        if (scan->socket == c->_hTCP) {
            return scan;
        }
    }
    return NULL;
}

void TelnetServer::addClient(Client *c) {
    TelnetClient *scan;
    TelnetClient *nc = (TelnetClient *)malloc(sizeof(TelnetClient));
    nc->next = NULL;
    nc->socket = c->_hTCP;
    nc->buffer = (char *)malloc(TELNET_BUFSZ);
    nc->phase = 0;
    nc->echo = true;
    nc->prompt = NULL;
    nc->buffer[0] = 0;
    setPrompt(nc, "> ");
    sendIAC(c, DONT, 34);
    sendIAC(c, DONT, 1);
    sendIAC(c, WILL, 1);

    if (_clients == NULL) {
        _clients = nc;
    } else {
        for (scan = _clients; scan->next; scan = scan->next);
        scan->next = nc;
    }

    if (_connectHandler != NULL) {
        _connectHandler(nc);
    }

    c->print(nc->prompt);
}

void TelnetServer::serveClient(TelnetClient *c) {
    Client cli(c->socket);

    if (!cli.connected()) {
        deleteClient(c);
        return;
    }

    if (!cli.available()) {
        return;
    }

    int ch = cli.read();
    if (c->keypressHandler == NULL) {
        int blen = strlen(c->buffer);
        switch (ch) {
            case '\r':
                cli.println();
                // find and parse command
                if (!executeCommand(c)) {
                    cli.println("Unknown Command");
                }
                cli.print(c->prompt);
                c->buffer[0] = 0;
                break;
            case '\n':
                break;
            case 8:
            case 127:
                if (blen > 0) {
                    blen--;
                    c->buffer[blen] = 0;
                    cli.write(8);
                    cli.write(' ');
                    cli.write(8);
                }
                break;
            case 255:
                if (c->phase == 1) {
                    c->buffer[blen] = 255;
                    c->buffer[blen+1] = 0;
                    c->phase = 0;
                } else {
                    c->phase = 1;
                    c->iac_cmd = 0;
                    c->iac_code = 0;
                }
                break;
            default:
                if (c->phase == 1) {
                    if (c->iac_cmd == 0) {
                        c->iac_cmd = ch;
                    } else {
                        c->iac_code = ch;
                        if (c->iac_cmd == DO) {
                            switch (c->iac_code) {
                                case 1:
                                    sendIAC(&cli, WILL, 1);
                                    break;
                                case 3:
                                    sendIAC(&cli, WILL, 3);
                                    break;
                                default:
                                    sendIAC(&cli, WONT, c->iac_code);
                            }
                        }
                        if (c->iac_cmd == WILL) {
                            sendIAC(&cli, DONT, c->iac_code);
                        }
                        c->phase = 0;
                    }
                } else {
                    if (blen < TELNET_BUFSZ-1) {
                        c->buffer[blen] = ch;
                        c->buffer[blen+1] = 0;
                        if (c->echo) {
                            cli.write(ch);
                        }
                    }
                }
                break;
        }
    } else {
        c->keypressHandler(c, ch);
    }
}

void TelnetServer::deleteClient(TelnetClient *c) {
    TelnetClient *scan;

    if (_clients == c) {
        scan = _clients;
        _clients = _clients->next;
        free(scan->buffer);
        free(scan);
        return;
    }

    for (scan = _clients; scan->next; scan = scan->next) {
        if (scan->next == c) {
            scan->next = scan->next->next;
            free(c->buffer);
            free(c);
            return;
        }
    }
}

void TelnetServer::sendIAC(Client *c, uint8_t cmd, uint8_t type) {
    c->write(IAC);
    c->write(cmd);
    c->write(type);
}

void TelnetServer::setPrompt(TelnetClient *c, char *p) {
    if (c->prompt != NULL) {
        free(c->prompt);
    }
    c->prompt = strdup(p);
}

void TelnetServer::setConnectHandler(void (*func)(TelnetClient *)) {
    _connectHandler = func;
}

void TelnetServer::addCommand(char *command, int (*function)(TelnetClient *, int argc, char **argv)) {
    TelnetCommand *scan;
    TelnetCommand *nc = (TelnetCommand *)malloc(sizeof(TelnetCommand));
    nc->command = strdup(command);
    nc->function = function;
    nc->next = NULL;

    if (_commands == NULL) {
        _commands = nc;
        return;
    }

    for (scan = _commands; scan->next; scan = scan->next);
    scan->next = nc;
}

boolean TelnetServer::executeCommand(TelnetClient *c) {

    if (c->inputHandler != NULL) {
        c->inputHandler(c);
        return true;
    }

    // First we need to estimate the number of slices to allocate
    // This can never be any more than one more than the number
    // of spaces or tabs in the command line.

    char *scan;
    int count = 1;
    TelnetCommand *cmdscan;

    int argc;
    char **argv;

    for (scan = c->buffer; *scan; scan++) {
        if (*scan == ' ' || *scan == '\t') {
            count++;
        }
    }

    argv = (char **)malloc(sizeof(char *) * count);
    argc = 0;
    scan = strtok(c->buffer, " ");
    while (scan) {
        argv[argc] = scan;
        argc++;
        scan = strtok(NULL, " ");
    };

    if (argc == 0) {
        free(argv);
        return true;
    }

    for (cmdscan = _commands; cmdscan; cmdscan = cmdscan->next) {
        if (!strcmp(cmdscan->command, argv[0])) {
            cmdscan->function(c, argc, argv);
            free(argv);
            return true;
        }
    }
    free(argv);
    return false;
}

void TelnetServer::disconnect(TelnetClient *c) {
    Client cli(c->socket);
    cli.stop();
    deleteClient(c);
}

void TelnetServer::clearConnectHandler() {
    _connectHandler = NULL;
}

void TelnetServer::setKeypressHandler(TelnetClient *c, void (*function)(TelnetClient *, int)) {
    c->keypressHandler = function;
}

void TelnetServer::setInputHandler(TelnetClient *c, void (*function)(TelnetClient *)) {
    c->inputHandler = function;
}

void TelnetServer::clearKeypressHandler(TelnetClient *c) {
    c->keypressHandler = NULL;
}

void TelnetServer::clearInputHandler(TelnetClient *c) {
    c->inputHandler = NULL;
}

void TelnetServer::setEcho(TelnetClient *c, boolean echo) {
    c->echo = echo;
}

