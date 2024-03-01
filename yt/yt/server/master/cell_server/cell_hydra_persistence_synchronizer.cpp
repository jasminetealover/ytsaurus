#include "cell_hydra_persistence_synchronizer.h"

#include "private.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>

#include <yt/yt/server/master/tablet_server/config.h>

#include <yt/yt/server/lib/cellar_agent/helpers.h>

#include <yt/yt/server/lib/tablet_server/proto/tablet_manager.pb.h>

#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/rpc/dispatcher.h>

#include <yt/yt/core/ypath/helpers.h>

namespace NYT::NCellServer {

using namespace NApi;
using namespace NCellMaster;
using namespace NCellarAgent;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NElection;
using namespace NObjectClient;
using namespace NTabletServer;
using namespace NTabletServer::NProto;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CellServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TCellHydraPersistenceSynchronizer
    : public ICellHydraPersistenceSynchronizer
{
public:
    explicit TCellHydraPersistenceSynchronizer(TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
        , DynamicConfig_(New<TDynamicCellHydraPersistenceSynchronizerConfig>())
    {
        YT_VERIFY(Bootstrap_);
        const auto& configManager = Bootstrap_->GetConfigManager();
        configManager->SubscribeConfigChanged(BIND(&TCellHydraPersistenceSynchronizer::OnDynamicConfigChanged, MakeWeak(this)));
    }

    void Start() override
    {
        YT_VERIFY(!PeriodicExecutor_);
        PeriodicExecutor_ = New<TPeriodicExecutor>(
            NRpc::TDispatcher::Get()->GetHeavyInvoker(),
            BIND(&TCellHydraPersistenceSynchronizer::OnSynchronize, MakeWeak(this)),
            GetDynamicConfig()->SynchronizationPeriod);
        PeriodicExecutor_->Start();
    }

    void Stop() override
    {
        if (PeriodicExecutor_) {
            YT_UNUSED_FUTURE(PeriodicExecutor_->Stop());
            PeriodicExecutor_.Reset();
        }
    }

private:
    TBootstrap* const Bootstrap_;
    TPeriodicExecutorPtr PeriodicExecutor_;
    TAtomicIntrusivePtr<TDynamicCellHydraPersistenceSynchronizerConfig> DynamicConfig_;

    using TPeerListPtr = IListNodePtr;

    void OnDynamicConfigChanged(const TDynamicClusterConfigPtr& /*oldConfig*/)
    {
        const auto& newConfig = Bootstrap_->GetConfigManager()->GetConfig()->TabletManager->CellHydraPersistenceSynchronizer;
        DynamicConfig_.Store(newConfig);

        if (PeriodicExecutor_) {
            PeriodicExecutor_->SetPeriod(newConfig->SynchronizationPeriod);
        }
    }

    TDynamicCellHydraPersistenceSynchronizerConfigPtr GetDynamicConfig() const
    {
        return DynamicConfig_.Acquire();
    }

    struct TCellInfo
    {
        TTabletCellOptionsPtr Options;
        int Version;
        TPeerListPtr Peers;
    };

    THashMap<TCellId, TCellInfo> GetCellInfoForCells(
        const std::vector<TCellId>& cellIds)
    {
        auto proxy = CreateObjectServiceReadProxy(
            Bootstrap_->GetRootClient(),
            EMasterChannelKind::Follower);

        THashMap<TCellId, TCellInfo> cellIdToCellInfo;
        THashMap<TCellBundleId, std::vector<TCellId>> cellBundleIdToCellIds;

        // Fetch cell bundle ids.
        {
            auto batchReq = proxy.ExecuteBatch(cellIds.size());
            for (auto cellId : cellIds) {
                auto req = TYPathProxy::Get(FromObjectId(cellId) + "/@cell_bundle_id");
                req->Tag() = cellId;
                batchReq->AddRequest(req);
            }
            auto batchRsp = WaitFor(batchReq->Invoke())
                .ValueOrThrow();
            for (const auto& [tag, rspOrError] : batchRsp->GetTaggedResponses<TYPathProxy::TRspGet>()) {
                auto cellId = std::any_cast<TCellId>(tag);
                if (!rspOrError.IsOK()) {
                    YT_LOG_WARNING(rspOrError,
                        "Error fetching cell bundle id for cell (CellId: %v)",
                        cellId);
                    continue;
                }
                auto cellBundleId = ConvertTo<TCellBundleId>(TYsonString(rspOrError.Value()->value()));
                cellBundleIdToCellIds[cellBundleId].push_back(cellId);
            }
        }

        // Fetch cell options with version.
        {
            auto batchReq = proxy.ExecuteBatch(cellBundleIdToCellIds.size());
            for (auto cellBundleId : GetKeys(cellBundleIdToCellIds)) {
                auto req = TYPathProxy::Get(FromObjectId(cellBundleId) + "/@");
                ToProto(req->mutable_attributes()->mutable_keys(), std::vector<TString>{
                    "options",
                    "config_version",
                });
                req->Tag() = cellBundleId;
                batchReq->AddRequest(req);
            }
            auto batchRsp = WaitFor(batchReq->Invoke())
                .ValueOrThrow();
            for (const auto& [tag, rspOrError] : batchRsp->GetTaggedResponses<TYPathProxy::TRspGet>()) {
                auto cellBundleId = std::any_cast<TCellBundleId>(tag);
                if (!rspOrError.IsOK()) {
                    YT_LOG_WARNING(rspOrError,
                        "Error fetching cell bundle attributes (CellBundleId: %v)",
                        cellBundleId);
                    continue;
                }
                auto attributes = ConvertToAttributes(TYsonString(rspOrError.Value()->value()));
                auto options = attributes->GetAndRemove<TTabletCellOptionsPtr>("options");
                auto version = attributes->Get<int>("config_version");

                auto it = cellBundleIdToCellIds.find(cellBundleId);
                YT_VERIFY(it != cellBundleIdToCellIds.end());
                const auto& cellIds = it->second;
                for (auto cellId : cellIds) {
                    EmplaceOrCrash(cellIdToCellInfo, cellId, TCellInfo{
                        .Options = options,
                        .Version = version,
                    });
                }
            }
        }

        // Fetch independent peers.
        {
            auto batchReq = proxy.ExecuteBatch(cellIdToCellInfo.size());
            for (const auto& [cellId, cellInfo] : cellIdToCellInfo) {
                if (cellInfo.Options->IndependentPeers) {
                    auto req = TYPathProxy::Get(FromObjectId(cellId) + "/@peers");
                    req->Tag() = cellId;
                    batchReq->AddRequest(req);
                }
            }
            auto batchRsp = WaitFor(batchReq->Invoke())
                .ValueOrThrow();
            for (const auto& [tag, rspOrError] : batchRsp->GetTaggedResponses<TYPathProxy::TRspGet>()) {
                auto cellId = std::any_cast<TCellId>(tag);
                auto it = cellIdToCellInfo.find(cellId);
                YT_VERIFY(it != cellIdToCellInfo.end());
                if (!rspOrError.IsOK()) {
                    YT_LOG_WARNING(rspOrError,
                        "Error fetching peers for cell (CellId: %v)",
                        cellId);
                    cellIdToCellInfo.erase(it);
                    continue;
                }
                auto node = ConvertToNode(TYsonString(rspOrError.Value()->value()));
                it->second.Peers = node->AsList();
            }
        }

        return cellIdToCellInfo;
    }

    TFuture<void> RegisterCellInCypress(
        TCellId cellId,
        const TPeerListPtr& peers,
        const TTabletCellOptionsPtr& cellOptions)
    {
        YT_LOG_DEBUG("Registering cell in Cypress (CellId: %v)",
            cellId);

        auto cellNodePath = GetCellHydraPersistencePath(cellId);
        auto proxy = CreateObjectServiceWriteProxy(Bootstrap_->GetRootClient());
        auto batchReq = proxy.ExecuteBatch();

        {
            auto req = TCypressYPathProxy::Create(cellNodePath);
            req->set_type(ToProto<int>(EObjectType::MapNode));
            req->set_ignore_existing(true);
            batchReq->AddRequest(req);
        }

        return batchReq->Invoke()
            .Apply(BIND([=] (const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError) mutable {
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));

                auto createAttributes = [&] (const auto& acl) {
                    auto attributes = CreateEphemeralAttributes();
                    attributes->Set("inherit_acl", false);
                    attributes->Set("acl", acl);
                    return attributes;
                };

                auto snapshotAttributes = createAttributes(cellOptions->SnapshotAcl);
                auto changelogAttributes = createAttributes(cellOptions->ChangelogAcl);

                auto batchReq = proxy.ExecuteBatch();

                auto createSnapshotAndChangelogNodes = [&] (const TYPath& path) {
                    // Create "snapshots" child.
                    {
                        auto req = TCypressYPathProxy::Create(path + "/snapshots");
                        req->set_type(ToProto<int>(EObjectType::MapNode));
                        req->set_ignore_existing(true);
                        ToProto(req->mutable_node_attributes(), *snapshotAttributes);
                        batchReq->AddRequest(req);
                    }
                    // Create "changelogs" child.
                    {
                        auto req = TCypressYPathProxy::Create(path + "/changelogs");
                        req->set_type(ToProto<int>(EObjectType::MapNode));
                        req->set_ignore_existing(true);
                        ToProto(req->mutable_node_attributes(), *changelogAttributes);
                        batchReq->AddRequest(req);
                    }
                };

                if (peers) {
                    // NB: to avoid race, peers' map nodes must be created first.
                    auto batchReq = proxy.ExecuteBatch();
                    for (int peerId = 0; peerId < peers->GetChildCount(); ++peerId) {
                        auto peer = peers->GetChildOrThrow(peerId)->AsMap();
                        if (peer->GetChildValueOrDefault("alien", false)) {
                            continue;
                        }

                        auto req = TCypressYPathProxy::Create(YPathJoin(cellNodePath, peerId));
                        req->set_type(ToProto<int>(EObjectType::MapNode));
                        req->set_ignore_existing(true);
                        req->Tag() = peerId;
                        batchReq->AddRequest(req);
                    }
                    auto batchRsp = WaitFor(batchReq->Invoke())
                        .ValueOrThrow();
                    for (const auto& [tag, rspOrError] : batchRsp->GetTaggedResponses<TCypressYPathProxy::TRspCreate>()) {
                        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError);
                        auto peerId = std::any_cast<int>(tag);
                        createSnapshotAndChangelogNodes(YPathJoin(cellNodePath, peerId));
                    }
                } else {
                    createSnapshotAndChangelogNodes(cellNodePath);
                }

                return batchReq->Invoke()
                    .Apply(BIND(ThrowCumulativeErrorIfFailed));
            }).AsyncVia(GetCurrentInvoker()));
    }

    void RegisterCellsInCypress(
        const std::vector<TCellId>& cellIds,
        TReqOnCellsHydraPersistenceSynchronized* request)
    {
        if (cellIds.empty()) {
            return;
        }

        auto proxy = CreateObjectServiceReadProxy(
            Bootstrap_->GetRootClient(),
            EMasterChannelKind::Follower);

        auto cellIdToCellInfo = GetCellInfoForCells(cellIds);
        auto activeCellIds = GetKeys(cellIdToCellInfo);

        std::vector<TFuture<void>> futures;
        futures.reserve(cellIdToCellInfo.size());
        for (const auto& [cellId, cellInfo] : cellIdToCellInfo) {
            futures.push_back(RegisterCellInCypress(
                cellId,
                cellInfo.Peers,
                cellInfo.Options));
        }
        auto results = WaitFor(AllSet(std::move(futures)))
            .ValueOrThrow();

        for (int index = 0; index < std::ssize(activeCellIds); ++index) {
            auto cellId = activeCellIds[index];
            const auto& result = results[index];
            if (!result.IsOK()) {
                YT_LOG_WARNING(result,
                    "Error registering cell in Cypress (CellId: %v)",
                    cellId);
                continue;
            }
            ToProto(request->add_cypress_registered_ids(), cellId);
        }
    }

    void UnregisterCellsFromCypress(const std::vector<TCellId>& cellIds)
    {
        if (cellIds.empty()) {
            return;
        }

        auto proxy = CreateObjectServiceWriteProxy(Bootstrap_->GetRootClient());
        auto batchReq = proxy.ExecuteBatch();
        for (auto cellId : cellIds) {
            YT_LOG_INFO("Unregistering cell from Cypress (CellId: %v)",
                cellId);

            auto path = GetCellHydraPersistencePath(cellId);
            auto req = TYPathProxy::Remove(path);
            req->set_force(true);
            req->set_recursive(true);
            batchReq->AddRequest(req);
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        auto cumulativeError = GetCumulativeError(batchRspOrError);
        THROW_ERROR_EXCEPTION_IF_FAILED(cumulativeError);
    }

    TFuture<void> UpdateCellHydraPersistenceAcls(
        TCellId cellId,
        const TPeerListPtr& peers,
        const TTabletCellOptionsPtr& cellOptions)
    {
        YT_LOG_DEBUG("Executing cell ACLs update (CellId: %v)",
            cellId);

        auto snapshotAcl = ConvertToYsonString(cellOptions->SnapshotAcl, EYsonFormat::Binary).ToString();
        auto changelogAcl = ConvertToYsonString(cellOptions->ChangelogAcl, EYsonFormat::Binary).ToString();

        auto proxy = CreateObjectServiceWriteProxy(Bootstrap_->GetRootClient());
        auto batchReq = proxy.ExecuteBatch();

        auto updateAcl = [&] (const TYPath& path) {
            {
                auto req = TYPathProxy::Set(path + "/snapshots/@acl");
                req->set_value(snapshotAcl);
                batchReq->AddRequest(req);
            }
            {
                auto req = TYPathProxy::Set(path + "/changelogs/@acl");
                req->set_value(changelogAcl);
                batchReq->AddRequest(req);
            }
        };

        auto processStorage = [&] (const TYPath& path) {
            if (peers) {
                for (int peerId = 0; peerId < peers->GetChildCount(); ++peerId) {
                    auto peer = peers->GetChildOrThrow(peerId)->AsMap();
                    if (peer->GetChildValueOrDefault("alien", false)) {
                        continue;
                    }
                    auto peerNodePath = YPathJoin(path, peerId);
                    updateAcl(peerNodePath);
                }
            } else {
                updateAcl(path);
            }
        };

        processStorage(GetCellHydraPersistencePath(cellId));
        // COPMAT(danilalexeev)
        if (!GetDynamicConfig()->MigrateToVirtualCellMaps) {
            processStorage(GetCellPath(cellId));
        }

        return batchReq->Invoke()
            .Apply(BIND(ThrowCumulativeErrorIfFailed));
    }

    void ExecuteCellAclsUpdates(
        const std::vector<TCellId>& cellIds,
        TReqOnCellsHydraPersistenceSynchronized* request)
    {
        if (cellIds.empty()) {
            return;
        }

        auto cellIdToCellInfo = GetCellInfoForCells(cellIds);
        auto activeCellIds = GetKeys(cellIdToCellInfo);

        std::vector<TFuture<void>> futures;
        futures.reserve(cellIdToCellInfo.size());
        for (const auto& [cellId, cellInfo] : cellIdToCellInfo) {
            futures.push_back(UpdateCellHydraPersistenceAcls(
                cellId,
                cellInfo.Peers,
                cellInfo.Options));
        }
        auto results = WaitFor(AllSet(std::move(futures)))
            .ValueOrThrow();

        for (int index = 0; index < std::ssize(activeCellIds); ++index) {
            auto cellId = activeCellIds[index];
            const auto& result = results[index];
            auto version = cellIdToCellInfo[cellId].Version;
            if (!result.IsOK()) {
                YT_LOG_WARNING(result,
                    "Error updating cell ACLs (CellId: %v)",
                    cellId);
                continue;
            }
            auto* updateInfo = request->add_acls_update_info();
            ToProto(updateInfo->mutable_cell_id(), cellId);
            updateInfo->set_config_version(version);
        }
    }

    void OnSynchronize()
    {
        auto dynamicConfig = GetDynamicConfig();
        if (!dynamicConfig->UseHydraPersistenceDirectory) {
            return;
        }

        YT_LOG_DEBUG("Synchronizing cells Hydra presistence");

        auto proxy = CreateObjectServiceReadProxy(
            Bootstrap_->GetRootClient(),
            EMasterChannelKind::Follower);

        THashSet<TCellId> aliveCellIds;
        THashSet<TCellId> registeredCellIds;
        std::vector<TCellId> toRegisterCellIds;
        std::vector<TCellId> toUnregisterCellIds;
        std::vector<TCellId> pendingAclsUpdateCellIds;

        try {
            auto batchReq = proxy.ExecuteBatch();
            batchReq->AddRequest(TYPathProxy::List(TabletCellsHydraPersistenceCypressPrefix));
            batchReq->AddRequest(TYPathProxy::List(ChaosCellsHydraPersistenceCypressPrefix));
            auto batchRsp = WaitFor(batchReq->Invoke())
                .ValueOrThrow();

            for (const auto& rspOrError : batchRsp->GetResponses<TYPathProxy::TRspList>()) {
                auto listNode = ConvertToNode(TYsonString(rspOrError.ValueOrThrow()->value()));
                auto list = listNode->AsList();
                for (const auto& item : list->GetChildren()) {
                    auto cellId = ConvertTo<TCellId>(item);
                    EmplaceOrCrash(registeredCellIds, cellId);
                }
            }
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex,
                "Error listing registered cells");
            return;
        }

        try {
            auto batchReq = proxy.ExecuteBatch();
            auto listAliveCells = [&] (const TYPath& path) {
                auto req = TYPathProxy::List(path);
                ToProto(req->mutable_attributes()->mutable_keys(), std::vector<TString>{
                    "registered_in_cypress",
                    "pending_acls_update",
                });
                batchReq->AddRequest(req);
            };
            listAliveCells(TabletCellCypressPrefix);
            listAliveCells(ChaosCellCypressPrefix);
            auto batchRsp = WaitFor(batchReq->Invoke())
                .ValueOrThrow();

            for (const auto& rspOrError : batchRsp->GetResponses<TYPathProxy::TRspList>()) {
                auto listNode = ConvertToNode(TYsonString(rspOrError.ValueOrThrow()->value()));
                auto list = listNode->AsList();
                for (const auto& item : list->GetChildren()) {
                    auto cellId = ConvertTo<TCellId>(item);
                    EmplaceOrCrash(aliveCellIds, cellId);
                    if (!item->Attributes().Get<bool>("registered_in_cypress", false) &&
                        std::ssize(toRegisterCellIds) < dynamicConfig->MaxCellsToRegisterInCypressPerIteration)
                    {
                        toRegisterCellIds.push_back(cellId);
                    }
                    if (item->Attributes().Get<bool>("pending_acls_update", false) &&
                        std::ssize(pendingAclsUpdateCellIds) < dynamicConfig->MaxCellAclsUpdatesPerIteration)
                    {
                        pendingAclsUpdateCellIds.push_back(cellId);
                    }
                }
            }
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex,
                "Error listing alive cells");
            return;
        }

        TReqOnCellsHydraPersistenceSynchronized request;

        for (auto cellId : registeredCellIds) {
            if (!aliveCellIds.contains(cellId) &&
                std::ssize(toUnregisterCellIds) < dynamicConfig->MaxCellsToUnregisterFromCypressPerIteration)
            {
                toUnregisterCellIds.push_back(cellId);
            }
        }

        try {
            RegisterCellsInCypress(toRegisterCellIds, &request);
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex,
                "Error registering cells in Cypress");
        }

        try {
            UnregisterCellsFromCypress(toUnregisterCellIds);
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex,
                "Error unregistering cells from Cypress");
        }

        try {
            ExecuteCellAclsUpdates(pendingAclsUpdateCellIds, &request);
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex,
                "Error executing cell ACLs updates");
        }

        if (request.cypress_registered_ids_size() != 0 ||
            request.acls_update_info_size() != 0)
        {
            auto future = CreateMutation(Bootstrap_->GetHydraFacade()->GetHydraManager(), request)
                ->CommitAndLog(Logger);
            Y_UNUSED(WaitFor(future));
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

ICellHydraPersistenceSynchronizerPtr CreateCellHydraPersistenceSynchronizer(
    NCellMaster::TBootstrap* bootstrap)
{
    return New<TCellHydraPersistenceSynchronizer>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellServer