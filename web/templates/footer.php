    </main>

    <!-- Footer -->
    <footer class="footer mt-auto py-3 bg-light">
        <div class="container-fluid">
            <div class="d-flex justify-content-between align-items-center">
                <span class="text-muted">
                    CariTranscoder v<?php echo CARITRANS_VERSION; ?> &copy; <?php echo date('Y'); ?> CariTech Solutions
                </span>
                <span class="text-muted small">
                    <span id="footer-time"><?php echo date('H:i:s'); ?></span> |
                    Node: <?php echo htmlspecialchars(gethostname()); ?>
                </span>
            </div>
        </div>
    </footer>

    <!-- Toast Container for Notifications -->
    <div class="toast-container position-fixed bottom-0 end-0 p-3" id="toast-container"></div>

    <!-- Confirmation Modal -->
    <div class="modal fade" id="confirmModal" tabindex="-1">
        <div class="modal-dialog">
            <div class="modal-content">
                <div class="modal-header">
                    <h5 class="modal-title" id="confirmModalTitle">Confirm Action</h5>
                    <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
                </div>
                <div class="modal-body" id="confirmModalBody">
                    Are you sure?
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="button" class="btn btn-primary" id="confirmModalOk">Confirm</button>
                </div>
            </div>
        </div>
    </div>

    <script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/js/bootstrap.bundle.min.js"></script>
    <script src="js/caritrans.js"></script>
    <script>
        // Update footer time
        setInterval(() => {
            const now = new Date();
            document.getElementById('footer-time').textContent =
                now.toTimeString().split(' ')[0];
        }, 1000);

        // CSRF token for AJAX requests
        const csrfToken = '<?php echo auth_get_csrf_token(); ?>';
    </script>
</body>
</html>
