#include "02_rollback_simulation/rollback_client.h"
#include "02_rollback_simulation/rollback_server.h"
#include "asteroid/packet_type.h"
#include "engine/engine.h"

#include "imgui.h"

namespace neko::net
{
SimulationClient::SimulationClient(SimulationServer& server) :
	server_(server), gameManager_(*this)
{
}

void SimulationClient::Init()
{
	const auto& config = BasicEngine::GetInstance()->config;
	windowSize_ = config.windowSize;
	gameManager_.SetWindowSize(windowSize_ / Vec2u(2, 1));
	framebuffer_.SetSize(windowSize_ / Vec2u(2, 1));
	framebuffer_.Create();

	clientId_ = RandomRange(std::numeric_limits<ClientId>::lowest(),
		std::numeric_limits<ClientId>::max());
	//JOIN packet
	gameManager_.Init();


}

void SimulationClient::Update(seconds dt)
{
	gameManager_.Update(dt);
}



void SimulationClient::Destroy()
{
	framebuffer_.Destroy();
	gameManager_.Destroy();

}

void SimulationClient::Render()
{
	const auto& config = BasicEngine::GetInstance()->config;

	if (config.windowSize != windowSize_)
	{
		windowSize_ = config.windowSize;
		framebuffer_.SetSize(windowSize_ / Vec2u(2, 1));
		framebuffer_.Reload();
		gameManager_.SetWindowSize(windowSize_ / Vec2u(2, 1));

	}

	framebuffer_.Bind();
	framebuffer_.Clear(Vec3f(0.0f));
	gameManager_.Render();
	gl::Framebuffer::Unbind();


}


void SimulationClient::SetPlayerInput(net::PlayerInput playerInput)
{
    auto currentFrame = gameManager_.GetCurrentFrame();
	gameManager_.SetPlayerInput(
		gameManager_.GetPlayerNumber(),
		playerInput, 
		currentFrame);

}

void SimulationClient::DrawImGui()
{
	const auto windowName = "Client "+std::to_string(clientId_);
    ImGui::Begin(windowName.c_str());
    if(gameManager_.GetPlayerNumber() == INVALID_PLAYER && ImGui::Button("Spawn Player"))
    {
        auto joinPacket = std::make_unique<asteroid::JoinPacket>();
        joinPacket->packetType = asteroid::PacketType::JOIN;
        auto* clientIdPtr = reinterpret_cast<std::uint8_t*>(&clientId_);
        for(int i = 0; i < sizeof(clientId_); i++)
        {
            joinPacket->clientId[i] = clientIdPtr[i];
        }
        SendPacket(std::move(joinPacket));
    }
    gameManager_.DrawImGui();
    ImGui::End();
}

void SimulationClient::SendPacket(std::unique_ptr<asteroid::Packet> packet)
{
    server_.ReceivePacket(std::move(packet));
}

void SimulationClient::ReceivePacket(const asteroid::Packet* packet)
{
    const auto packetType = static_cast<asteroid::PacketType>(packet->packetType);
    switch (packetType)
    {
        case asteroid::PacketType::SPAWN_PLAYER:
        {
            const auto* spawnPlayerPacket = static_cast<const asteroid::SpawnPlayerPacket*>(packet);
            ClientId clientId = spawnPlayerPacket->clientId[0];
            clientId += spawnPlayerPacket->clientId[1] << 8u;

            const PlayerNumber playerNumber = spawnPlayerPacket->playerNumber;
            if (clientId == clientId_)
            {
                gameManager_.SetClientPlayer(playerNumber);
            }

            Vec2f pos;
            auto* posPtr = reinterpret_cast<std::uint8_t*>(&pos[0]);
            for(size_t i = 0; i < sizeof(Vec2f);i++)
            {
                posPtr[i] = spawnPlayerPacket->pos[i];
            }
            degree_t rotation;
            auto* rotationPtr = reinterpret_cast<std::uint8_t*>(&rotation);
            for(size_t i = 0; i < sizeof(degree_t); i++)
            {
                rotationPtr[i] = spawnPlayerPacket->angle[i];
            }
            gameManager_.SpawnPlayer(playerNumber, pos, rotation);
            break;
        }
        case asteroid::PacketType::START_GAME:
        {
            const auto* startGamePacket = static_cast<const asteroid::StartGamePacket*>(packet);
            long startingTime = 0;
            auto* ptr = reinterpret_cast<std::uint8_t*>(&startingTime);
            for(int i = 0; i < sizeof(startingTime); i++)
            {
                ptr[i] = startGamePacket->startTime[i];
            }
            gameManager_.StartGame(startingTime);
            break;
        }
        case asteroid::PacketType::INPUT:
        {
            const auto* playerInputPacket = static_cast<const asteroid::PlayerInputPacket*>(packet);
            const auto playerNumber = playerInputPacket->playerNumber;
            net::Frame inputFrame = 0;
            auto* inputPtr = reinterpret_cast<std::uint8_t*>(&inputFrame);
            for(size_t i = 0; i < sizeof(net::Frame);i++)
            {
                inputPtr[i] = playerInputPacket->currentFrame[i];
            }
            if(playerNumber == gameManager_.GetPlayerNumber())
            {
                //Verify the inputs coming back from the server
                const auto& inputs = gameManager_.GetRollbackManager().GetInputs(playerNumber);
                const auto currentFrame = gameManager_.GetRollbackManager().GetCurrentFrame();
                for(size_t i = 0; i < playerInputPacket->inputs.size(); i++)
                {
                    const auto index = currentFrame-inputFrame+i;
                    if(index > inputs.size())
                    {
                        break;
                    }
                    if(inputs[index] != playerInputPacket->inputs[i])
                    {
                        neko_assert(false, "Inputs coming back from server are not coherent!!!");
                    }
                    if(inputFrame - i == 0)
                    {
                        break;
                    }
                }
                break;
            }

            //discard delayed input packet
            if(inputFrame < gameManager_.GetRollbackManager().GetLastReceivedFrame(playerNumber))
            {
                break;
            }
            for(int i = 0; i < playerInputPacket->inputs.size(); i++)
            {
                gameManager_.SetPlayerInput(playerNumber, 
                    playerInputPacket->inputs[i], 
                    inputFrame - i);
            	
                if(inputFrame-i == 0)
                {
                    break;
                }
            }
            break;
        }
        case asteroid::PacketType::VALIDATE_STATE:
        {
        	
            const auto* validateFramePacket = static_cast<const asteroid::ValidateFramePacket*>(packet);
            Frame newValidateFrame = 0;
            auto* framePtr = reinterpret_cast<std::uint8_t*>(&newValidateFrame);
        	for(size_t i = 0; i < sizeof(Frame);i++)
        	{
                framePtr[i] = validateFramePacket->newValidateFrame[i];
        	}
        	std::array<asteroid::PhysicsState, asteroid::maxPlayerNmb> physicsStates{};
        	for(size_t i = 0; i < validateFramePacket->physicsState.size(); i++)
            {
        	    auto* statePtr = reinterpret_cast<std::uint8_t*>(physicsStates.data());
        	    statePtr[i] = validateFramePacket->physicsState[i];
            }
            gameManager_.ConfirmValidateFrame(newValidateFrame, physicsStates);
            //logDebug("Client received validate frame " + std::to_string(newValidateFrame));
        	break;
        }
        case asteroid::PacketType::SPAWN_BULLET: break;
        default: ;
    }
}
}
